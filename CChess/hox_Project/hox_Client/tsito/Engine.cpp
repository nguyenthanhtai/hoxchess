#include <strstream>
#include <vector>
#include <stack>

#include "Engine.h"
#include "Board.h"
#include "Evaluator.h"
#include "Lawyer.h"
#include "Move.h"
#include "Transposition.h"
#include "OpeningBook.h"


#define OPENING_BOOK_FILE "book.dat"

/* Engine.cpp (c) Noah Roberts 2003-02-27
 */
using namespace std;

enum { SEARCH_AB, SEARCH_PV, SEARCH_NS };
enum { SEARCHING, DONE_SEARCHING, BETWEEN_SEARCHES };
enum { NO_ABORT, ABORT_TIME, ABORT_READ };

Engine::Engine( Board *brd, Lawyer *law )
            : board( brd)
            , lawyer( law )
{
    evaluator     = Evaluator::defaultEvaluator();
    _openingBook  = NULL;
    transposTable = new TranspositionTable(18);

    // Options and their defaults...
    maxPly            = /* HPHAN: 6 */ 2;
    quiesc            = false;
    qnull             = false;
    qhash             = true;
    _useOpeningBook   = /* HPHAN: true */ false;
    _displayThinking  = false;
    searchMethod      = SEARCH_AB;
    verifyNull        = true;
    nullMoveReductionFactor = 3;
    useMTDF           = true;
    allowNull         = true;
    useIterDeep       = true;
    allowTableWindowAdjustments = false;
    useTable          = true;

    _searchAborted    = NO_ABORT;
    _searchState      = BETWEEN_SEARCHES;
    Timer::setTimers(0, 0, 0); // Shut off timers by default.

    // Register with Options class
    Options::defaultOptions()->addObserver(this);

    // Open opening book if ok to do so.
    if (_useOpeningBook)
    {
        _openingBook = new OpeningBook( OPENING_BOOK_FILE );
        if (!_openingBook->valid()) // if not a valid book...
        {
            delete _openingBook;
            _openingBook = NULL;
            cerr << "Can't open my book!\n";
        }
    }
}

Engine::~Engine()
{
    delete transposTable;
    delete _openingBook;
}

// Move sort comparison function and required variables...
static Board *_board = NULL;
static vector<Move> priorityTable;
bool compareMoves(Move &m1, Move &m2)
{
  Evaluator *e = Evaluator::defaultEvaluator();
  int m1Index = -1, m2Index = -1;
  for (size_t i = 0; i < ::priorityTable.size(); i++)
    {
      if (::priorityTable[i] == m1) m1Index = i;
      if (::priorityTable[i] == m2) m2Index = i;
    }
  if (m1Index == -1 && m2Index != -1) return false;
  if (m2Index == -1 && m1Index != -1) return true;
  if (m1Index < m2Index) return true;
  if (m2Index < m1Index) return false;

  // Now that killer moves and transposition table are taken into account we try to order other moves logically

  // king moves last...
  if (_board->pieceAt(m1.origin()) == JIANG && _board->pieceAt(m2.origin()) != JIANG) return false;
  if (_board->pieceAt(m2.origin()) == JIANG && _board->pieceAt(m1.origin()) != JIANG) return true;

  // Captures: MVV/LVA (Most Valuable Victem/Least Valuable Attacker)
  if (e->pieceValue(_board->pieceAt(m1.destination())) < e->pieceValue(_board->pieceAt(m2.destination()))) return false;
  if (e->pieceValue(_board->pieceAt(m1.destination())) > e->pieceValue(_board->pieceAt(m2.destination()))) return true;
  if (e->pieceValue(_board->pieceAt(m1.origin())) == e->pieceValue(_board->pieceAt(m2.origin()))) return false;
  if (e->pieceValue(_board->pieceAt(m1.origin())) < e->pieceValue(_board->pieceAt(m2.origin())))
    {
      if (_board->pieceAt(m1.destination()) == EMPTY)
        return false;
      else
        return true;
    }
  else
    {
      if (_board->pieceAt(m1.destination()) == EMPTY)
        return true;
      else
        return false;
    }
}

// Adds killer move to the queue.
void Engine::newKiller(Move& theMove, int ply)
{
  Move temp;
  if (killer1.size() > (size_t) ply)
    {
      temp = killer1[ply];
      killer1[ply] = theMove;
      if (killer2.size() > (size_t) ply)
        killer2[ply] = temp;
      else
        killer2.push_back(theMove);
    }
  else
    killer1.push_back(theMove);
}

// Implementation of MTD(f)...
long Engine::mtd(std::vector<PVEntry> &pv, long guess, int depth)
  /* Inputs : pv, estimated best score, max depth.
     Outputs: pv, actual best score. */
{
  /*
   * NOTE: it appears that null move heuristics cause too many troubles when used with
   *       this algorithm.  Therefor null move will not be used when using mtd. */
  long g = guess;
  long upperbound = INFIN;
  long lowerbound = -INFIN;
  long beta = 0;
  long best = -INFIN;
  vector<PVEntry> tempPV;
  do
    {
      pv.clear(); // Clear the principle variation...

      // Move window.
      if (g == lowerbound) beta = g+1;
      else beta = g;

      // Search a window with 0 thickness - all searches fail...
      g = search(pv, beta-1, beta, 0, depth, false, true, true);

      // slide boundaries
      if (g < beta) upperbound = g;
      else lowerbound = g;
      
    } while (lowerbound < upperbound); // When lower is >= upper then we have zeroed in on
                                       // the best score.


  return g;
}

/**
 * Inputs : NONE
 * Outputs: score of primary line and a filled principle variation.
 */
long
Engine::think()
{
    if (_searchState == DONE_SEARCHING)
    {
        cerr << "Shouldn't be happening: think()\n";
        return 0;
    }

    myTimer = Timer::timerForColor(board->sideToMove());

    if (_searchState == BETWEEN_SEARCHES)
    {
        _searchState = SEARCHING;
        _principleVariation.clear();
        //myTimer->startTimer();
        transposTable->flush();
        killer1.clear();
        killer2.clear();
        ftime(&startTime);
        nodeCount = 0;
    }

    // Before we do ANYTHING else, check if the current position is recorded in the opening
    // database, if so then use the move returned (if legal).
    if ( _openingBook != NULL && _openingBook->valid() )
    {
        unsigned short moveNum = _openingBook->getMove(board);
        if (moveNum)
        {
            Move m((int)(moveNum >> 8), (int)(moveNum & 255));
            if (   !(m.origin() == 0 && m.destination() == 0) // should never fail
                && lawyer->legalMove(m) ) // maybe book file has illegal moves in it.
            {
                _principleVariation.push_back(PVEntry(m,NO_CUTOFF));
                //myTimer->stopTimer();
                _searchState = DONE_SEARCHING;
                return evaluator->evaluatePosition(*board, *lawyer);
            }
        }
    }

    // Clear everything that was set by the previous search
    // Estimate outcome...evaluate current board.
    long result = evaluator->evaluatePosition(*board, *lawyer);

    _searchAborted = NO_ABORT;
  
    for ( int i = ( useIterDeep ? (_principleVariation.size()+1)
                                : maxPly );
              i <= maxPly && !_searchAborted;
              ++i )
    {
        vector<PVEntry> iterPV;
        result = (useMTDF ? mtd((useIterDeep ? iterPV: _principleVariation),result,i)
                          : search((useIterDeep ? iterPV: _principleVariation), -INFIN,INFIN,0,i));
        if (!_searchAborted && useIterDeep)
        {
            _principleVariation = iterPV;
        }
        if (_displayThinking)
        {
            struct timeb nowTime;
            int plyMod = (_searchAborted ? -1:0);
            ftime(&nowTime);
            cout << (_searchAborted ? "*":"") << (i+plyMod) << "\t" << result << "\t" << nodeCount << "\t"
                 << (100*(nowTime.time - startTime.time)) + ((nowTime.millitm - startTime.millitm)/10) << "\t"
                 << variationText(_principleVariation) << endl;
            cout.flush();
        }
    }

    // We may continue searching if there is data to be read...depends on what we read.
    if (_searchAborted != ABORT_READ) // a) finished, b) timed out - either way we are done.
    {   // Search is completed then.
        //myTimer->stopTimer();
        _searchState = DONE_SEARCHING;
    }
  
  return result;
}

Move Engine::getMove()
{
    _searchState = BETWEEN_SEARCHES;
    if (_principleVariation.size() > 0) // should never not be when called.
        return _principleVariation[0].move;
    else
        return Move();
}

void Engine::optionChanged(string whatOption)
{
  if (whatOption == "searchPly")
    {
      string x = Options::defaultOptions()->getValue(whatOption);
      istrstream strm((char*)x.c_str(), x.length());
      strm >> maxPly;
    }
  else if (whatOption == "quiescence")
    {
      if (Options::defaultOptions()->getValue(whatOption) == "on")
        quiesc = true;
      else
        quiesc = false;
    }
  else if (whatOption == "useOpeningBook")
    {
      if (Options::defaultOptions()->getValue(whatOption) == "n")
        _useOpeningBook = false;
      else
        _useOpeningBook = true;
    }
  else if (whatOption == "search")
    {
      string type = Options::defaultOptions()->getValue(whatOption);
      if (type == "mtd")
        {
          useMTDF = true;
          searchMethod = SEARCH_AB;
        }
      else if (type == "alphabeta")
        {
          useMTDF = false;
          searchMethod = SEARCH_AB;
        }
      else if (type == "negascout")
        {
          useMTDF = false;
          searchMethod = SEARCH_NS;
        }
    }
  else if (whatOption == "nullmove")
    {
      if (Options::defaultOptions()->getValue(whatOption) == "on")
        allowNull = true;
      else
        allowNull = false;
    }
  else if (whatOption == "verifynull")
    {
      if (Options::defaultOptions()->getValue(whatOption) == "on")
        {
          allowNull = true;
          verifyNull = true;
          nullMoveReductionFactor = 3;
        }
      else
        {
          verifyNull = false;
          nullMoveReductionFactor = 2;
        }
    }
  else if (whatOption == "hash")
    {
      if (Options::defaultOptions()->getValue(whatOption) == "on")
        useTable = true;
      else
        useTable = false;
    }
  else if (whatOption == "hashadjust")
    {
      if (Options::defaultOptions()->getValue(whatOption) == "on")
        allowTableWindowAdjustments = true;
      else
        allowTableWindowAdjustments = false;
    }
  else if (whatOption == "iterative")
    {
      if (Options::defaultOptions()->getValue(whatOption) == "on")
        useIterDeep = true;
      else
        useIterDeep = false;
      //_searchState = DONE_SEARCHING; // This one changes the search in an incompatable fassion.
      _searchState = BETWEEN_SEARCHES;
    }
  else if (whatOption == "post")
    {
      if (Options::defaultOptions()->getValue(whatOption) == "on")
        _displayThinking = true;
      else
        _displayThinking = false;
    }
  else if (whatOption == "qnull")
    {
      if (Options::defaultOptions()->getValue(whatOption) == "on")
        qnull = true;
      else
        qnull = false;
    }
  else if (whatOption == "qhash")
    {
      if (Options::defaultOptions()->getValue(whatOption) == "on")
        qhash = true;
      else
        qhash = false;
    }
  else if (whatOption == "computerColor")
    _searchState = BETWEEN_SEARCHES;
}


void Engine::filterOutNonCaptures(list<Move> &moveList)
{
  if (lawyer->inCheck()) return; // In check positions we want to look at all legal moves.

  list<Move> copy;

  // Otherwise only captures.
  for (list<Move>::iterator it = moveList.begin(); it != moveList.end();)
    {
      if (board->pieceAt(it->destination()) == EMPTY)
        {
          break; // captures are on top (assuming sorted) so we can just stop processing here.
        }
      else
        {
          copy.push_back(*it);
          it++;
        }
    }
    moveList = copy; // This will be faster because there will be fewer moves...usually
}

std::string
Engine::variationText(const vector<PVEntry>& pv) const
{
    std::string	var;

    for (vector<PVEntry>::const_iterator it = pv.begin(); it != pv.end(); ++it)
    {
        var += it->move.getText() + " ";
    }

    if (pv.size() > 0)
    {
        switch (pv[pv.size() - 1].cutoff)
        {
          case HASH_CUTOFF:  var += "{HT}";   break;
          case NULL_CUTOFF:  var += "{NM}";   break;
          case MATE_CUTOFF:  var += "(MATE}"; break;
          case MISC_CUTOFF:  var += "{MC}";   break;
        }
    }  

    return var;
}

/* Search algorithms - the basics - all use move ordering */


long Engine::search(vector<PVEntry> &pv, long alpha, long beta, int ply, int depth,
                    bool legalonly, bool nullOk, bool verify)
  /* Inputs : alpha (lower bound), beta (upper bound), ply (depth so far),
              depth (target depth), legalonly (only generate legal moves - expensive),
              nullOk (it is ok to try null move), verify (verify beta cutoff on null)

     Outputs: primary variation (pv), score of line to play for side to move.

     This function does everything that is generic to the search, seperate from specific
     search algorithms.
  */
{
  vector<PVEntry> myPV;
  long value = 0;
  bool failHigh = false;
  ::_board = board;
  list<Move>		moveList;
  int			pvPreSize = pv.size();

  if (_searchAborted) return 0;

  // Check if we have time left...
  if (!myTimer->haveTimeLeftForMove())
    {
      _searchAborted = ABORT_TIME;
      return -INFIN;
    }

  ::priorityTable.clear();

  nodeCount++;

  // Look for check on either side and preform appropriate action.
  if (lawyer->inCheck((board->sideToMove() == RED ? BLUE:RED)))
    {
      if (!pv.empty()) pv[pv.size()-1].cutoff = MISC_CUTOFF;
      return INFIN; // I can take the king right now...illegal move made.
    }
  //if (lawyer->gameWonByChase() != NOCOLOR || lawyer->gameWonByPCheck() != NOCOLOR) return (-CHECKMATE) - ply;
  //if ((!legalonly) && lawyer->inCheck()) legalonly = true; // I am in check.

  // Return evaluation if this is a leaf node.
  if (ply >= depth)
    {
      if (quiesc)
        return quiescence(alpha,beta,ply,depth,nullOk);
      else
        return evaluator->evaluatePosition(*board, *lawyer) - ply;
    }

  Move m;
  long score;
  if (tableSearch(ply, depth, alpha, beta, m, score, nullOk))
    {
      pv.push_back(PVEntry(m, HASH_CUTOFF));
      return score;
    }

  verify = (verify && verifyNull);
  nullOk = (allowNull && nullOk);

  // Perform null move if appropriate.
  if (ply > 0 && (!legalonly) && nullOk && ((!verify) || ((depth-ply) > 2)))
    {
      vector<PVEntry> ignore;
      board->makeNullMove();
      value = -search(ignore, -beta, 1-beta, ply+1, depth-nullMoveReductionFactor,
                      false, false, verify);
      board->unmakeMove();

      if (value > beta)
        {
          if (verify) // For verified null move algorithm (Tabibi, Netanyahu 2002)
            {
              // Search the rest of the tree with a reduced depth just to be sure
              // we are not in zugzwang (move to loose).
              depth-=2;
              verify = false;  // Use unverified null moves for rest of search
              failHigh = true; // flag for later use
              nullOk = false;
            }
          else
            {
              if (!pv.empty()) pv[pv.size()-1].cutoff = NULL_CUTOFF;
              return value;
            }
        }
    }

  setUpKillers(ply);
  
  lawyer->generateMoves(moveList, legalonly);
  if (moveList.empty())
    {
      if (!pv.empty()) pv[pv.size()-1].cutoff = MATE_CUTOFF;
      return CHECKMATE + ply;
    }
  moveList.sort(::compareMoves); // The information to sort has been set up by "search".


  
research: // Goto usually bad, but simplifies the code here.
  switch (searchMethod)
    /* Call the specific search algorithm the user wants us to use. */
    {
    case SEARCH_AB:
      value = alphaBeta(myPV, moveList, alpha, beta, ply, depth, legalonly, nullOk, verify);
      break;
    case SEARCH_PV:
      //value = pvSearch(pv, moveList, alpha, beta, ply, depth, legalonly, nullOk, verify);
      break;
    case SEARCH_NS:
      value = negaScout(myPV, moveList, alpha, beta, ply, depth, legalonly, nullOk, verify);
      break;
    }
  if (_searchAborted)
    {
      pv.insert(pv.end(), myPV.begin(), myPV.end());
      return value; // Don't want timed out search trees to get into the table.
    }

  // This is straight out of the text on Verified Null-Move.
  if ((failHigh) && (value < beta)) // flag is set and our best move is not that great.
    {
      myPV.clear();
      depth++;
      failHigh = false;
      verify = true;
      goto research;  // Re-do search, zugzwang position caused beta cutoff on null when
                      // the best move is NOT above beta.  These are rare cases so the cost
                      // is not that bad...the gain in worth it.
    }

  if (value > beta && !myPV.empty())
    newKiller(myPV[0].move, ply);

  if (!myPV.empty())
    tableSet(ply, depth, alpha, beta, myPV[0].move, value);

  pv.insert(pv.end(), myPV.begin(), myPV.end());
  return value;
}

// Quescence version of search - enough is altered to warrent a different method.
long Engine::quiescence(long alpha, long beta, int ply, int depth, bool nullOk)
    /* Inputs : alpha, beta, ply, depth, nullOk
       Outputs: score.

       This function is a special case of the search that only searches captures and
       check evasions.  It only evaluates when it reaches a quiet position; this happens
       when there is nothing to capture and the side to move is not in check or a null
       cutoff occurs.
    */
{
  long value = 0;
  bool failHigh = false;
  ::_board = board;
  list<Move>	moveList;
  bool		legalonly = false;
  vector<PVEntry> myPV;
  bool verify = false;

  if (!myTimer->haveTimeLeftForMove())
    {
      _searchAborted = ABORT_TIME;
      return -INFIN;
    }

  // Look for check on either side and preform appropriate action.
  if (lawyer->inCheck((board->sideToMove() == RED ? BLUE:RED)))
    {
      return INFIN; // I can take the king right now...illegal move made.
    }
  //if (lawyer->gameWonByChase() != NOCOLOR || lawyer->gameWonByPCheck() != NOCOLOR) return (-CHECKMATE) - ply;
  //if ((!legalonly) && lawyer->inCheck()) legalonly = true; // I am in check.

  Move m;
  long score;
  if (qhash && tableSearch(ply, depth, alpha, beta, m, score, nullOk))
    {
      return score;
    }
  
  if (!legalonly && nullOk && qnull)
    {
      vector<PVEntry> ignore;
      board->makeNullMove();
      quiesc = false;
      // Search to depth 1 looking for a beta cutoff - no quiescence during null.
      value = -search(ignore, -beta, 1-beta, 0,1, false, false, verify);
      quiesc = true;
      board->unmakeMove();

      // This position is quiet, return evaluation.
      if (value >= beta) return evaluator->evaluatePosition(*board,*lawyer);
    }
  

  lawyer->generateMoves(moveList, legalonly);
  if (moveList.empty()) return CHECKMATE;
  
  setUpKillers(ply);
  moveList.sort(::compareMoves);

  // Only pay attention to captures and evasions.
  filterOutNonCaptures(moveList);
  // If no capture moves exist then list is empty, this is a quiet position prime for
  // evaluation.
  if (moveList.empty()) return evaluator->evaluatePosition(*board, *lawyer);

  switch (searchMethod)
    /* Call the specific search algorithm the user wants us to use. */
    {
    case SEARCH_AB:
      value = alphaBeta(myPV, moveList, alpha, beta, ply, depth, legalonly, nullOk, verify);
      break;
    case SEARCH_PV:
      //value = pvSearch(pv, moveList, alpha, beta, ply, depth, legalonly, nullOk, verify);
      break;
    case SEARCH_NS:
      value = negaScout(myPV, moveList, alpha, beta, ply, depth, legalonly, nullOk, verify);
      break;
    }
  if (_searchAborted) return value;
  if (value > beta && !myPV.empty())
    newKiller(myPV[0].move, ply);

  if (qhash && !myPV.empty())
    tableSet(ply, depth, alpha, beta, myPV[0].move, value);

  return value;
}

// Traditional AlphaBeta Search...
long Engine::alphaBeta(vector<PVEntry> &pv, list<Move> moveList, long alpha, long beta,
                       int ply, int depth, bool legalonly, bool nullOk,
                       bool verify)
  /* Inputs : moveList (list of moves to search), alpha (lower bound), beta (upper),
              ply (current depth), depth (target depth), legalonly (not used),
              nullOk (not used), verify (passed on).

     Outputs: principle variation and score for side to move.
  */
{
  vector<PVEntry>	myPV; // Holds the best PV while we search.

 
  long best = -INFIN;//alpha - 1;
  long value     = 0;

  // Iterate through the moves to find the best one.
  for (list<Move>::iterator it = moveList.begin();
       it != moveList.end() && best < beta; // have moves and haven't surpassed beta
       it++)
    // We stop searching at beta because that means this line is worse for the other side
    // than any that came before, so they won't make that move and so this line never
    // takes place.  We assume the other player makes the best move possible.
    {
      vector<PVEntry> tempPV;
      tempPV.push_back(PVEntry(*it,NO_CUTOFF));
      board->makeMove(*it);
      if (best > alpha)  alpha = best; // We don't care about any lines that score less than
                                       // our current best.
      value = -search(tempPV, -beta, -alpha, ply+1, depth, false, true, verify);
      //value -= ply;

      if (value > -INFIN && value > best) // found one that is better.
        {
          best = value;  // replace best value
          myPV = tempPV; // replace current PV.
        }
      board->unmakeMove();
    }
  if (best == -INFIN) return CHECKMATE;
  pv.insert(pv.end(), myPV.begin(), myPV.end()); // add best line to PV.
  return best; // return value of line.
}


// Principle Variation Search...


// NegaScout Search... best first algorithm that searches the first move with full
// window and then the rest with 0 width windows just to prove the first was the best.
// If the first is not the best then we research the one that gave us a better score
// with window between beta and the returned score to find a true value.
long Engine::negaScout(std::vector<PVEntry> &pv,  std::list<Move> moveList,
                       long alpha, long beta, int ply, int depth,
                       bool legalonly, bool nullOk, bool verify)
  /* Inputs : moveList (list of moves to search), alpha (lower bound), beta (upper),
              ply (current depth), depth (target depth), legalonly (not used),
              nullOk (not used), verify (passed on).

     Outputs: principle variation and score for side to move.
  */
{
  // NOTE: not currently working with implementation of MTD(f) - not used when that alg
  //       selected.  End up with empty or incomplete lines.  NegaScout adds no benefit
  //       with MTD(f) anyway since the supplied window is null - NegaScout deteriorates into
  //       AlphaBeta at that point but doesn't work right.
  vector<PVEntry>	myPV;

  long a = alpha;
  long b = beta;
  long t = 0;


  for (list<Move>::iterator it = moveList.begin(); it != moveList.end() && a < beta; it++)
    {
      vector<PVEntry> tempPV;
      tempPV.push_back(PVEntry(*it, NO_CUTOFF));
      board->makeMove(*it);
      t = -search(tempPV, -b, -a, ply+1, depth, false, true, verify);
      if (t > a && t < beta && it != moveList.begin() && ply < depth-1)
        // Best move was not best - must search again.
        {
          tempPV.clear();
          tempPV.push_back(PVEntry(*it, NO_CUTOFF));
          a = -search(tempPV, -beta, -t, ply+1, depth, false, true, verify);
        }
      board->unmakeMove();
      if (t > a) // We have a better best.
        {
          a = t;
          myPV = tempPV;
        }
      b = a+1; // From now on searches are done with 0 width window.
    }
  pv.insert(pv.end(), myPV.begin(), myPV.end());
  return a;
}

void Engine::setUpKillers(int ply)
{
  if (killer1.size() > (size_t) (ply-1))
    ::priorityTable.push_back(killer1[ply-1]);
  if (killer2.size() > (size_t) (ply-1))
    ::priorityTable.push_back(killer2[ply-1]);
}


// looks for position in table.
bool Engine::tableSearch(int ply, int depth, long &alpha, long &beta, Move &m, long &score,
                         bool &nullok)
{
  if (!useTable) return false;
  TNode searchNode;
  transposTable->find(board, searchNode);
  if (searchNode.flag() != NOT_FOUND)
    {
      m = searchNode.move();
      score = searchNode.score();
      if (allowTableWindowAdjustments)
        {
          switch (searchNode.flag())
            {
            case EXACT_SCORE: break;
            case UPPER_BOUND:
              if (beta > score && score > alpha)
                {
                  beta = score;
                  nullok = false;
                }
              break;
            case LOWER_BOUND:
              if (alpha < score && score < beta)
                {
                  alpha = score;
                  nullok = false;
                }
              break;
            }
        }
      ::priorityTable.push_back(m);
      if (searchNode.depth() > (depth-ply) && (ply > 0 || searchNode.flag() == EXACT_SCORE))
        return true;
      return false;
    }
  return false;
}
// stores position in table.
void Engine::tableSet(int ply, int depth, long alpha, long beta, Move m, long score)
{
  if (!useTable) return;
  TNode	storeNode(board);
  storeNode.move(m);
  storeNode.score(score);
  storeNode.depth(depth-ply);
  
  if (score < alpha) // UPPER BOUND
      storeNode.flag(UPPER_BOUND);
  else if (score < beta) // EXACT_VALUE
      storeNode.flag(EXACT_SCORE);
  else // LOWER BOUND
      storeNode.flag(LOWER_BOUND);

  transposTable->store(storeNode);
}

void Engine::endSearch()    { _searchState = DONE_SEARCHING; }
bool Engine::doneThinking() { return _searchState == DONE_SEARCHING; }
bool Engine::thinking()     { return _searchState == SEARCHING; }
