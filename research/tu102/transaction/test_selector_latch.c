/* Explicit local entry-latch policy, all simulated registers/real fixtures. */
#define TX_ENTRY_LATCHED_LOCAL
#define main inherited_loader_tests
#include "test_core.c"
#undef main

int main(void) {
    (void)inherited_loader_tests(); /* same core tests under latch policy */
    tx.pool=malloc(TX_POOL_SIZE);assert(tx.pool);
    unsigned checks=0;
    const uint32_t pairs[][2]={{0x03000001,3},{0x62004020,2},{0x12345678,0},{0,15},{0x77777777,8}};
    for(unsigned i=0;i<sizeof(pairs)/sizeof(pairs[0]);i++) {
        setup();model.ss0=pairs[i][0];model.ss1=pairs[i][1];
        assert(tx_guards(&tx)==TX_OK && tx.baseline_valid);
        assert(tx.initial_ss0==model.ss0 && tx.initial_ss1==model.ss1);checks++;
        for(unsigned word=0;word<2;word++)for(unsigned bit=0;bit<32;bit++) {
            setup();model.ss0=pairs[i][0];model.ss1=pairs[i][1];assert(tx_guards(&tx)==TX_OK);
            if(word)model.ss1^=1U<<bit;else model.ss0^=1U<<bit;
            assert(tx_run(&tx)!=TX_OK && !model.writes && !model.fw_starts);checks++;
        }
    }
    const uint32_t invalid[][2]={{0xffffffff,0},{0xbadf1234,0},{0x88888888,0},{0x12345678,16},{0x12345678,0xffffffff}};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        setup();model.ss0=invalid[i][0];model.ss1=invalid[i][1];
        assert(tx_guards(&tx)!=TX_OK && !tx.baseline_valid && !model.writes);checks++;
    }
    free(tx.pool);
    printf("PASS %u extra immutable selector-latch checks; no hardware\n",checks);
    return 0;
}
