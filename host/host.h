// Chess System Tal Retro - PC harness entry points.
#pragma once
int uci_loop();
int search_bench(int depth, int hash_kb);
int nnue_test(int n, int h);
int nnue_bench(int n, int h, int depth, int hash_kb);
int pgn_fens(int argc, char** argv);
int nnue_eval_file(int argc, char** argv);
