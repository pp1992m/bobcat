typedef int Score; 
typedef int Depth;

class Search : public Runnable, public MoveSorter {
public:
	Search(Protocol* protocol, Game* game, Eval* eval, SEE* see, TranspositionTable* transt) {
		initialise(protocol, game, eval, see, transt, false);
	}

	Search(Game* game, Eval* eval, SEE* see, TranspositionTable* transt) {
		initialise(NULL, game, eval, see, transt, true);
	}

	int go(int wtime = 0, int btime = 0, int movestogo = 0, int winc = 0, int binc = 0, int movetime = 2000) {
		pos = game->pos; // updated in makeMove and unmakeMove from here on

		initialiseSearch(wtime, btime, movestogo, winc, binc, movetime);

		Score alpha = -MAXSCORE;
		Score beta = MAXSCORE;

		pos->generateMoves(this, 0, LegalMoves);

		while (!stop_search && search_depth < 96*2) {
			try {
				max_ply = 0;
				max_plies = max(32, search_depth/2 + 16);
				Score score = searchRoot(search_depth, alpha, beta);
				savePv();

				if (isEasyMove()) {
					break;
				}
				alpha = max(-MAXSCORE, score - 50);
				beta = min(MAXSCORE, score + 50);
				search_depth += 1*2;
			}
			catch (const int) {
				while (ply) {
					unmakeMove();
				}
				savePv();
			}
		}
		postInfo(0, -1, search_depth/2, max_ply, node_count, nodesPerSecond(), timeUsed(), transt->getLoad());
		stopped = true;
		return 0;
	}

	void stop() {
		stop_search = true;
	}

	bool isStopped() {
		return stopped;
	}

	virtual void run() {
		go();
	}

	virtual void newGame() {
	}

protected:
	void initialise(Protocol* protocol, Game* game, Eval* eval, SEE* see, TranspositionTable* transt, bool worker) {
		this->protocol = protocol;
		this->game = game;
		this->eval = eval;
		this->see = see;
		this->transt = transt;
		board = game->pos->board;
		this->worker = worker;
		stopped = false;
	}

	void postInfo(const Move curr_move, int curr_move_number, int depth, int selective_depth, uint64 node_count, 
		uint64 nodes_per_sec, uint64 time, int hash_full)
	{
		if (!worker) {
			protocol->postInfo(0, -1, search_depth/2, max_ply, node_count, nodesPerSecond(), timeUsed(), transt->getLoad());
		}
	}

	Score searchRoot(const Depth depth, Score alpha, Score beta) {
		while (1) {
			pv_length[0] = 0;
			pos->eval_score = eval->evaluate();
			findTranspositionRefineEval(depth, alpha, beta);
			sortRootMoves(false);
			
			Move best_move = 0;
			Score best_score = -MAXSCORE, orig_alpha = alpha;
			int move_count = 0;

			while (const MoveData* move_data = pos->nextMove()) {
				const Move m = move_data->move;

				if (makeMove(m)) {
					Depth next_depth = getNextDepth(true, true, depth, ++move_count, 5, m, alpha, move_data->score);

					if (search_depth > 10*2 && search_time > 5000) {
						postInfo(m, move_count, search_depth/2, max_ply, node_count, nodesPerSecond(), timeUsed(), 
							transt->getLoad());
					}

					Score score;
					if (move_count == 1) {
						score = searchNextDepth(next_depth, -beta, -alpha, true, true);
					}
					else {
						score = searchNextDepth(next_depth, -alpha - 1, -alpha, false, false);
						if (score > alpha && score < beta) {
							score = searchNextDepth(next_depth, -beta, -alpha, true, true);
						}
					}
					if (score > alpha && next_depth < depth - 1*2) {
						score = searchNextDepth(depth - 1*2, -beta, -alpha, move_count == 1, true);
					}

					unmakeMove();

					if (score > best_score) {
						best_score = score;
						if (best_score > alpha) { 
							best_move = m;
							if (score >= beta) {
								break;
							}
							updatePv(m, best_score, depth);
							alpha = best_score; 
						}
					}
				}
			}
			pos->gotoMove(0);
			if (!move_count) { 
				return searchNodeScore(pos->in_check ? -MAXSCORE : 0);
			}
			else if (best_move) {
				updateHistoryScores(best_move, depth);
				if (best_score >= beta) {
					updateKillerMoves(best_move);
				}
				else {
					return searchNodeScore(best_score);
				}
			}
			storeTransposition(depth, best_score, nodeType(best_score, orig_alpha, beta), best_move);
			alpha = -MAXSCORE;
			beta = MAXSCORE;
		}
	}

	void sortRootMoves(bool use_root_move_scores) {
		int move_count = 0;
		while (MoveData* move_data = pos->nextMove()) {
			if (use_root_move_scores) {
				move_data->score = root_move_score[move_count++];
			}
			else {
				sortMove(*move_data);
			}
		}
		pos->gotoMove(0);
	}

	__forceinline Score searchNextDepth(const Depth depth, const Score alpha, const Score beta, bool is_left_most_node, 
		bool is_pv_node) 
	{
		return depth <= 0 ? -searchQuiesce(alpha, beta) : -searchAll(depth, alpha, beta, is_left_most_node, is_pv_node);
	}

 	Score searchAll(const Depth depth, Score alpha, const Score beta, const bool is_left_most_node, const bool is_pv_node) {
		if (game->isRepetition()) {
			return searchNodeScore(0);
		}
		
		findTranspositionRefineEval(depth, alpha, beta);

		if (!is_pv_node && pos->transp_score_valid && pos->transp_depth_valid) { 
			return searchNodeScore(pos->transp_score);
		} 
		
		if (ply >= max_plies - 1) {
			return searchNodeScore(pos->eval_score);
		}

		Score score;
		if (okToTryNullMove(depth, beta, is_pv_node)) {
			makeMove(0);
			score = searchNextDepth(depth - 4*2 - (depth/16)*2, -beta, -beta + 1, false, false);
			unmakeMove();
			if (score >= beta) {
				return searchNodeScore(score);
			}
		}

		if (!is_pv_node && !pos->in_check && depth <= 3*2) {
			if (pos->eval_score + 125 < alpha) {
				if (depth <= 1*2) {
					score = searchQuiesce(alpha, beta);
					return max(score, pos->eval_score + 125);
				}
				else {
					if (pos->eval_score + 400 < alpha) {
						if ((score = searchQuiesce(alpha, beta)) < alpha) {
							return max(score, pos->eval_score + 400);
						}
					}
				}
			}
		}

		if (!pos->transp_move && pos->eval_score >= beta - 100) {
			if (is_pv_node && depth >= 4*2) {
				score = searchAll(depth - 2*2, alpha, beta, true, true);
				findTranspositionRefineEval(depth, alpha, beta);
			}
			else if (!is_pv_node && depth >= 8*2) {
				score = searchAll(depth/2, alpha, beta, true, true);
				findTranspositionRefineEval(depth, alpha, beta);
			}
		}

		if (pos->in_check) {
			pos->generateMoves(this, 0, LegalMoves);
		}
		else {
			pos->generateMoves(this, pos->transp_move, Stages);
		}

		Move best_move = 0;
		Score best_score = -MAXSCORE, orig_alpha = alpha;
		int move_count = 0;

		while (const MoveData* move_data = pos->nextMove()) {
			const Move m = move_data->move;

			if (makeMove(m)) {
				Depth next_depth = getNextDepth(is_pv_node, is_left_most_node, depth, ++move_count, 5, m, alpha, 
					move_data->score);

				if (okToPruneLastMove(best_score, next_depth, depth, alpha, is_pv_node)) {
					unmakeMove();
					continue;
				}

				if (move_count == 1) {
					score = searchNextDepth(next_depth, -beta, -alpha, true, is_pv_node);
				}
				else {
					score = searchNextDepth(next_depth, -alpha - 1, -alpha, false, false);

					if (score > alpha && score < beta) {
						score = searchNextDepth(next_depth, -beta, -alpha, true, true);
					}
				}
				if (score > alpha && next_depth < depth - 1*2) {
					score = searchNextDepth(depth - 1*2, -beta, -alpha, move_count == 1, is_pv_node);
				}

				unmakeMove();

				if (score > best_score) {
					best_score = score;
					if (best_score > alpha) { 
						best_move = m;
						if (score >= beta) {
							break;
						}
						updatePv(m, best_score, depth);
						alpha = best_score; 
					}
				}
			}
		}
		if (!move_count) { 
			return searchNodeScore(pos->in_check ? -MAXSCORE + ply : 0);
		}
		else if (pos->reversible_half_move_count >= 100) { 
			return searchNodeScore(0);
		}
		else if (best_move) {
			updateHistoryScores(best_move, depth);
			if (best_score >= beta) {
				updateKillerMoves(best_move);
			}
		}
		return storeSearchNodeScore(best_score, depth, nodeType(best_score, orig_alpha, beta), best_move);
	}

	__forceinline bool okToTryNullMove(const Depth depth, const Score beta, const bool is_pv_node) const {
		return !pos->in_check 
			&& pos->null_moves_in_row < 1 
			&& depth > 1*2 
			&& !pos->material.hasPawnsOnly(pos->side_to_move) 
			&& pos->eval_score >= beta 
			//&& !is_pv_node
			;
	}

	__forceinline bool okToPruneLastMove(const Score best_score, const Depth next_depth, const Depth depth, const Score alpha, 
		const bool is_pv_node) const 
	{
		return next_depth <= 3*2  
			&& next_depth < depth - 1*2 // only prune when move is already reduced
			&& -pos->eval_score + fp_margin[max(0, next_depth)] < alpha 
			//&& !is_pv_node
			&& best_score > -MAXSCORE;
	}

	Score searchQuiesce(Score alpha, const Score beta, int qs_depth = 0) {
		findTransposition(0, alpha, beta);

		bool is_pv_node = beta - alpha > 1;
		if (!is_pv_node && pos->transp_score_valid && pos->transp_depth_valid) { 
			return searchNodeScore(pos->transp_score);
		} 

		if (pos->eval_score >= beta) {
			storeTransposition(0, pos->eval_score, BETA, 0);
			return searchNodeScore(pos->eval_score);
		}
		
		if (ply >= max_plies - 1) {
			return searchNodeScore(pos->eval_score);
		}

		if (qs_depth > 6) {
			return searchNodeScore(pos->eval_score);
		}
		
		Move best_move = 0;
		Score best_score = pos->eval_score, orig_alpha = alpha;
		int move_count = 0;

		if (best_score > alpha) {
			alpha = best_score; 
		}

		pos->generateCapturesAndPromotions(this);

		while (const MoveData* move_data = pos->nextMove()) {
			const Move m = move_data->move;

			if (!pos->in_check && !isPromotion(m)) {
				if (move_data->score < 0 || pos->eval_score + piece_value(CAPTURED(m)) + 150 < alpha) {
					continue;
				}
			}
			if (makeMove(m)) {
				move_count++;

				Score score = -searchQuiesce(-beta, -alpha, qs_depth + 1);
				unmakeMove();
				
				if (score > best_score) {
					best_score = score;
					if (best_score > alpha) { 
						best_move = m;
						if (score >= beta) {  
							break;
						}
						updatePv(m, best_score, 0);
						alpha = best_score; 
					}
				}
			}
		}
		if (!move_count) { 
			return best_score;
		}
		return storeSearchNodeScore(best_score, 0, nodeType(best_score, orig_alpha, beta), best_move);
	}

	__forceinline Depth getNextDepth(const bool is_pv_node, const bool is_left_most_node, const Depth depth, 
		const int move_count, const int L, const Move m, const Score alpha, const int move_score) 
	{
		if (pos->in_check) {
			return is_pv_node ? depth : (is_left_most_node ? depth - 1 : depth - 1*2);
		}

		if (!isPassedPawnMove(m) && !isQueenPromotion(m) && !isCapture(m) && move_count >= L) {
			return depth - 2*2 - (depth/16)*2 - max(0, ((move_count-6)/12)*2);
		}
		return depth - 1*2;
	}

	__forceinline bool makeMove(const Move m) {
		if (game->makeMove(m, false, false)) {
			pos = game->pos;
			Side us = pos->side_to_move, them = us ^ 1;	
			pos->eval_score = eval->evaluate();
			if (eval->attacks(us) & bb_square(board->king_square[them])) {
				game->unmakeMove();
				pos = game->pos;
				return false;
			}
			pos->in_check = (eval->attacks(them) & 
				bb_square(board->king_square[us])) != 0;

			ply++;
			if (ply > max_ply) {
				max_ply = ply;
			}
			pv_length[ply] = ply;
			node_count++;
			checkTime();
			return true;
		}
		return false;
	}

	__forceinline void unmakeMove() {
		game->unmakeMove();
		pos = game->pos;
		ply--;
	}

	__forceinline void checkTime() {
		if ((node_count & 0x3FFF) == 0) {
			if (!isAnalysing()) {
				stop_search = search_depth > 1*2 && timeUsed() > search_time;
			}
			else {
				if (protocol) protocol->checkInput();
			}	
			if (stop_search) {
				throw 1; 
			}
		}
		if ((node_count & 0x3FFFFF) == 0) {
			postInfo(0, -1, search_depth/2, max_ply, node_count, nodesPerSecond(), timeUsed(), transt->getLoad());
		}
	}

	__forceinline int isAnalysing() const {
		return protocol ? protocol->isAnalysing() : true;
	}

	__forceinline uint64 timeUsed() const {
		return max(1, millis() - start_time);
	}

	__forceinline void updatePv(const Move m, Score score, const Depth depth) {
		PVEntry* entry = &pv[ply][ply];

		entry->score = score;
		entry->depth = depth;
		entry->key = pos->key;
		entry->move = m;

		pv_length[ply] = pv_length[ply + 1];

		for (int i = ply + 1; i < pv_length[ply]; i++) { 
			pv[ply][i] = pv[ply + 1][i];
		}  
		if (ply == 0) {
			char buf[2048], buf2[16]; buf[0] = 0;
			for (int i = ply; i < pv_length[ply]; i++) { 
				_snprintf(&buf[strlen(buf)], sizeof(buf) - strlen(buf), "%s ", 
					moveToString(pv[ply][i].move, buf2));
			}
			if (!worker) {
				if (pos->material.key[pos->side_to_move] == 0x00000 && pos->material.key[pos->side_to_move ^ 1] == 0x00110) {
					if (score > -MAXSCORE / 2) {
						score += 250;
					}
				}
				protocol->postPv(search_depth/2, max_ply, node_count, nodesPerSecond(), timeUsed(), transt->getLoad(), score, 
					buf);
			}
		}
	}

	__forceinline void savePv() {
		if (pv_length[0]) {
			pos->pv_length = pv_length[0];
			for (int i = 0; i < pos->pv_length; i++) {
				const PVEntry& entry = pv[0][i];
				transt->insert(entry.key, entry.depth, entry.score, EXACT, entry.move);
			}
		}
	}

	uint64 nodesPerSecond() const {
		return (uint64)(node_count * 1000 / max(1, (millis() - start_time)));
	}

	__forceinline void updateHistoryScores(const Move m, const Depth depth) {
		history_scores[PIECE(m)][TO(m)] += (depth/2)*(depth/2);
		if (history_scores[PIECE(m)][TO(m)] > 50000) {
			for (int i = 0; i < 16; i++) for (int k = 0; k < 64; k++) {
				history_scores[i][k] >>= 2; 
			}
		}
	}

	__forceinline void updateKillerMoves(const Move m) {
		if (!isCapture(m) && !isPromotion(m) && m != killer_moves[0][ply]) {
			killer_moves[2][ply] = killer_moves[1][ply];
			killer_moves[1][ply] = killer_moves[0][ply];
			killer_moves[0][ply] = m;
		}
	}

	__forceinline Score codecTTableScore(Score score, Score ply) const {
		if (abs((int)score) < MAXSCORE - 128) {
			return score;
		}
		return score < 0 ? score - ply : score + ply;
	}

	void initialiseSearch(int wtime, int btime, int movestogo, int winc, int binc, int movetime) {
		if (!worker) {
			if (protocol->isFixedTime()) {
				search_time = (int)(0.9*(double)movetime);
			}
			else {
				int movesLeft = movestogo;
				if (movesLeft == 0) {
					movesLeft = 30;
				}

				time_left = pos->side_to_move == 0 ? wtime : btime;
				time_inc = pos->side_to_move == 0 ? winc : binc;

				search_time = time_left/movesLeft + time_inc;

				if (movestogo == 0) {
					if (time_left < 8000) {
						search_time /= 4;
					}
					else if (time_left < 16000) {
						search_time /= 2;
					}
					else if (time_left < 24000) {
						search_time = (int)(search_time/1.5);
					}
				}
				else {
					search_time = (int)(search_time*0.95);
				}
				if (search_time > time_left) {
					search_time = time_left/2;
				}
			}
			start_time = millis();
			transt->initialiseSearch();
		}
		search_depth = 1*2;
		memset(pv, 0, sizeof(pv));
		node_count = 1; 
		ply = 0;
		stop_search = false;
		pos->pv_length = 0;
		memset(root_move_score, 0, sizeof(root_move_score));
		memset(history_scores, 0, sizeof(history_scores)); 
		memset(killer_moves, 0, sizeof(killer_moves));
	}

	virtual void sortMove(MoveData& move_data) {
		const Move m = move_data.move;
		if (pos->transp_move == m) {
			move_data.score = 890010;
		}
		else if (isQueenPromotion(m)) {
			move_data.score = 890000;
		}
		else if (isCapture(m)) {
			Score value_captured = piece_value(CAPTURED(m));
			Score value_piece = piece_value(PIECE(m));
			if (value_piece == 0) {
				value_piece = 1800;
			}
			if (value_piece < value_captured || value_piece == value_captured) {
				move_data.score = 300000 + value_captured*20 - value_piece;
			}
			else if (value_piece == value_captured) {
				move_data.score = 240000 + value_captured;
			}
			else if (see->seeMove(m) >= 0) {
				move_data.score = 160000 + value_captured*20 - value_piece;
			}
			else { 
				move_data.score = -100000 + value_captured*20 - value_piece;
			}
		}
		else if (isPromotion(m)) {
			move_data.score = 70000 + piece_value(PROMOTED(m));
		}
		else if (m == killer_moves[0][ply]) {
			move_data.score = 125000;
		}
		else if (m == killer_moves[1][ply]) {
			move_data.score = 124999;
		}
		else if (m == killer_moves[2][ply]) {
			move_data.score = 124998;
		}
		else {
			move_data.score = history_scores[PIECE(m)][TO(m)];
		}
	}

	__forceinline Score searchNodeScore(const Score score) const {
		return score;
	}

	__forceinline Score storeSearchNodeScore(const Score score, const Depth depth, const int node_type, 
		const Move best_move) 
	{
		storeTransposition(depth, score, node_type, best_move);
		return searchNodeScore(score);
	}

	__forceinline void storeTransposition(const Depth depth, const Score score, const int node_type, const Move move) {
		pos->transposition = transt->insert(pos->key, depth, codecTTableScore(score, ply), node_type, move);
	}

	__forceinline void findTranspositionRefineEval(const Depth depth, const Score alpha, const Score beta) {
		if (!pos->transposition || pos->transposition->key != transt->key32(pos->key)) {
			if ((pos->transposition = transt->find(pos->key)) == 0) {
				pos->transp_score_valid = pos->transp_depth_valid = false;
				pos->transp_move = 0;
				return;
			}
		}
		pos->transp_depth_valid = pos->transposition->depth >= depth;
		pos->transp_score = codecTTableScore(pos->transposition->score, -ply);

		switch (pos->transposition->flags & 7) {
		case EXACT:
			pos->transp_score_valid = true;
			if (pos->transposition->depth) {
				pos->eval_score = pos->transp_score;
			}
			break;
		case BETA:
			if ((pos->transp_score_valid = pos->transp_score >= beta) && depth && pos->transposition->depth) {
				pos->eval_score = max(pos->eval_score, pos->transp_score);
			}
			break;
		case ALPHA:
			if ((pos->transp_score_valid = pos->transp_score <= alpha) && depth && pos->transposition->depth) {
				pos->eval_score = min(pos->eval_score, pos->transp_score);
			}
			break;
		default:
			pos->transp_score_valid = false;
			break;
		}
		pos->transp_move = pos->transposition->move;
	}

	__forceinline void findTransposition(const Depth depth, const Score alpha, const Score beta) {
		return findTranspositionRefineEval(depth, alpha, beta);
	}

	__forceinline int nodeType(const Score score, const Score orig_alpha, const Score beta) const {
		return score > orig_alpha ? (score >= beta ? BETA : EXACT) : ALPHA;
	}

	bool isEasyMove() const {
		if (pos->moveCount() == 1 && search_depth > 7) {
			return true;
		}
		return false;
	}

	__forceinline bool isPassedPawnMove(const Move& m) const {
		return (PIECE(m) & 7) == Pawn && board->isPawnPassed(TO(m), side(m));
	}

	__forceinline bool isPawnMove(const Move& m) const {
		return (PIECE(m) & 7) == Pawn;
	}

	struct PVEntry {
		uint64 key;
		Depth depth;
		Score score;
		Move move;
	};

public:
	Depth		ply;
	Depth		max_ply;
	PVEntry		pv[128][128];
	int			pv_length[128];
	uint64		start_time;
	uint64		search_time, time_left, time_inc;
	bool		stop_search;
	int			max_plies;

protected:	
	Depth		search_depth;
	bool		worker;
	Move		killer_moves[4][128];
	int			history_scores[16][64];
	Score		root_move_score[256];
	bool		stopped;
	Protocol*	protocol;
	Game*		game;
	Eval*		eval;
	Board*		board;
	SEE*		see;
	Position*	pos;
	TTable*		transt;

	static uint64 node_count;
	static int fp_margin[9];

	static const int EXACT = 1;
	static const int BETA = 2;
	static const int ALPHA = 4;
	static const int MAXSCORE = 0x7fff;
};

int Search::fp_margin[9] = { 150, 150, 150, 150, 400, 400, 400, 600, 600 };
uint64 Search::node_count;
