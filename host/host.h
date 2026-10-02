// Chess System Tal Retro - PC harness entry points.
#pragma once
int uci_loop();
int search_bench(int depth, int hash_kb);
int nnue_test(int n, int h);
int nnue_bench(int n, int h, int depth, int hash_kb);
