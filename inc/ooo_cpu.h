#ifndef OOO_CPU_H
#define OOO_CPU_H

#include <array>
#include <functional>

#include "champsim_constants.h"
#include "instruction.h"
#include "cache.h"
#include "instruction.h"

#define DEADLOCK_CYCLE 1000000

using namespace std;

#define STA_SIZE (ROB_SIZE*NUM_INSTR_DESTINATIONS_SPARC)

// cpu
class O3_CPU {
  public:
    uint32_t cpu = 0;

    // trace
    FILE *trace_file = NULL;
    char trace_string[1024];
    char gunzip_command[1024];

    // instruction
    input_instr current_instr;
    cloudsuite_instr current_cloudsuite_instr;
    uint64_t instr_unique_id = 0, completed_executions = 0,
             begin_sim_cycle = 0, begin_sim_instr,
             last_sim_cycle = 0, last_sim_instr = 0,
             finish_sim_cycle = 0, finish_sim_instr = 0,
             warmup_instructions, simulation_instructions, instrs_to_read_this_cycle = 0, instrs_to_fetch_this_cycle = 0,
             next_print_instruction = STAT_PRINTING_PERIOD, num_retired = 0;
    uint32_t inflight_reg_executions = 0, inflight_mem_executions = 0, num_searched = 0;
    uint32_t next_ITLB_fetch = 0;

    struct dib_entry_t
    {
        bool valid = false;
        unsigned lru = DIB_WAY;
        uint64_t addr = 0;
    };

    // instruction buffer
    using dib_t= std::array<std::array<dib_entry_t, DIB_WAY>, DIB_SET>;
    dib_t DIB;

    // reorder buffer, load/store queue, register file
    CORE_BUFFER<ooo_model_instr> IFETCH_BUFFER{"IFETCH_BUFFER", IFETCH_BUFFER_SIZE};
    CORE_BUFFER<ooo_model_instr> DECODE_BUFFER{"DECODE_BUFFER", DECODE_BUFFER_SIZE};
    CORE_BUFFER<ooo_model_instr> ROB{"ROB", ROB_SIZE};
    CORE_BUFFER<LSQ_ENTRY> LQ{"LQ", LQ_SIZE}, SQ{"SQ", SQ_SIZE};

    // store array, this structure is required to properly handle store instructions
    uint64_t STA[STA_SIZE], STA_head = 0, STA_tail = 0;

    // Ready-To-Execute
    uint32_t ready_to_execute[ROB_SIZE], ready_to_execute_head, ready_to_execute_tail;

    // Ready-To-Load
    uint32_t RTL0[LQ_SIZE], RTL0_head = 0, RTL0_tail = 0,
             RTL1[LQ_SIZE], RTL1_head = 0, RTL1_tail = 0;

    // Ready-To-Store
    uint32_t RTS0[SQ_SIZE], RTS0_head = 0, RTS0_tail = 0,
             RTS1[SQ_SIZE], RTS1_head = 0, RTS1_tail = 0;

    // branch
    int branch_mispredict_stall_fetch = 0; // flag that says that we should stall because a branch prediction was wrong
    int mispredicted_branch_iw_index = 0; // index in the instruction window of the mispredicted branch.  fetch resumes after the instruction at this index executes
    uint8_t  fetch_stall = 0;
    uint64_t fetch_resume_cycle = 0;
    uint64_t num_branch = 0, branch_mispredictions = 0;
    uint64_t total_rob_occupancy_at_branch_mispredict;

    uint64_t total_branch_types[8] = {};
    uint64_t branch_type_misses[8] = {};

    // TLBs and caches
    CACHE ITLB{"ITLB", ITLB_SET, ITLB_WAY, ITLB_WQ_SIZE, ITLB_RQ_SIZE, ITLB_PQ_SIZE, ITLB_MSHR_SIZE},
          DTLB{"DTLB", DTLB_SET, DTLB_WAY, DTLB_WQ_SIZE, DTLB_RQ_SIZE, DTLB_PQ_SIZE, DTLB_MSHR_SIZE},
          STLB{"STLB", STLB_SET, STLB_WAY, STLB_WQ_SIZE, STLB_RQ_SIZE, STLB_PQ_SIZE, STLB_MSHR_SIZE},
          L1I{"L1I", L1I_SET, L1I_WAY, L1I_WQ_SIZE, L1I_RQ_SIZE, L1I_PQ_SIZE, L1I_MSHR_SIZE},
          L1D{"L1D", L1D_SET, L1D_WAY, L1D_WQ_SIZE, L1D_RQ_SIZE, L1D_PQ_SIZE, L1D_MSHR_SIZE},
          L2C{"L2C", L2C_SET, L2C_WAY, L2C_WQ_SIZE, L2C_RQ_SIZE, L2C_PQ_SIZE, L2C_MSHR_SIZE};

  // trace cache for previously decoded instructions
  
    // constructor
    O3_CPU(uint32_t cpu, uint64_t warmup_instructions, uint64_t simulation_instructions) : cpu(cpu), begin_sim_cycle(warmup_instructions), warmup_instructions(warmup_instructions), simulation_instructions(simulation_instructions)
    {
        for (uint32_t i=0; i<STA_SIZE; i++)
	  STA[i] = UINT64_MAX;

        for (uint32_t i=0; i<ROB_SIZE; i++) {
	  ready_to_execute[i] = ROB_SIZE;
        }
        ready_to_execute_head = 0;
        ready_to_execute_head = 0;

        for (uint32_t i=0; i<LQ_SIZE; i++) {
	  RTL0[i] = LQ_SIZE;
	  RTL1[i] = LQ_SIZE;
        }

        for (uint32_t i=0; i<SQ_SIZE; i++) {
	  RTS0[i] = SQ_SIZE;
	  RTS1[i] = SQ_SIZE;
        }

        // BRANCH PREDICTOR & BTB
        initialize_branch_predictor();
	initialize_btb();

        // TLBs
        ITLB.cpu = this->cpu;
        ITLB.cache_type = IS_ITLB;
        ITLB.MAX_READ = 2;
        ITLB.MAX_WRITE = 2;
        ITLB.fill_level = FILL_L1;
        ITLB.extra_interface = &L1I;
        ITLB.lower_level = &STLB;

        DTLB.cpu = this->cpu;
        DTLB.cache_type = IS_DTLB;
        DTLB.MAX_READ = 2;
        DTLB.MAX_WRITE = 2;
        DTLB.fill_level = FILL_L1;
        DTLB.extra_interface = &L1D;
        DTLB.lower_level = &STLB;

        STLB.cpu = this->cpu;
        STLB.cache_type = IS_STLB;
        STLB.MAX_READ = 1;
        STLB.MAX_WRITE = 1;
        STLB.fill_level = FILL_L2;
        STLB.upper_level_icache[this->cpu] = &ITLB;
        STLB.upper_level_dcache[this->cpu] = &DTLB;

        // PRIVATE CACHE
        L1I.cpu = this->cpu;
        L1I.cache_type = IS_L1I;
        L1I.MAX_READ = 2;
        L1I.MAX_WRITE = 2;
        L1I.fill_level = FILL_L1;
        L1I.lower_level = &L2C;

        L1D.cpu = this->cpu;
        L1D.cache_type = IS_L1D;
        L1D.MAX_READ = 2;
        L1D.MAX_WRITE = 2;
        L1D.fill_level = FILL_L1;
        L1D.lower_level = &L2C;

        L2C.cpu = this->cpu;
        L2C.cache_type = IS_L2C;
        L2C.MAX_READ = 1;
        L2C.MAX_WRITE = 1;
        L2C.fill_level = FILL_L2;
        L2C.upper_level_icache[this->cpu] = &L1I;
        L2C.upper_level_dcache[this->cpu] = &L1D;

        l1i_prefetcher_initialize();
        L1D.l1d_prefetcher_initialize();
        L2C.l2c_prefetcher_initialize();

        using namespace std::placeholders;

        ITLB.find_victim = std::bind(&CACHE::lru_victim, &ITLB, _1, _2, _3, _4, _5, _6, _7);
        DTLB.find_victim = std::bind(&CACHE::lru_victim, &DTLB, _1, _2, _3, _4, _5, _6, _7);
        STLB.find_victim = std::bind(&CACHE::lru_victim, &STLB, _1, _2, _3, _4, _5, _6, _7);
        L1I.find_victim = std::bind(&CACHE::lru_victim, &L1I, _1, _2, _3, _4, _5, _6, _7);
        L1D.find_victim = std::bind(&CACHE::lru_victim, &L1D, _1, _2, _3, _4, _5, _6, _7);
        L2C.find_victim = std::bind(&CACHE::lru_victim, &L2C, _1, _2, _3, _4, _5, _6, _7);

        ITLB.update_replacement_state = std::bind(&CACHE::lru_update, &ITLB, _2, _3, _7, _8);
        DTLB.update_replacement_state = std::bind(&CACHE::lru_update, &DTLB, _2, _3, _7, _8);
        STLB.update_replacement_state = std::bind(&CACHE::lru_update, &STLB, _2, _3, _7, _8);
        L1I.update_replacement_state = std::bind(&CACHE::lru_update, &L1I, _2, _3, _7, _8);
        L1D.update_replacement_state = std::bind(&CACHE::lru_update, &L1D, _2, _3, _7, _8);
        L2C.update_replacement_state = std::bind(&CACHE::lru_update, &L2C, _2, _3, _7, _8);

        ITLB.replacement_final_stats = std::bind(&CACHE::lru_final_stats, &ITLB);
        DTLB.replacement_final_stats = std::bind(&CACHE::lru_final_stats, &DTLB);
        STLB.replacement_final_stats = std::bind(&CACHE::lru_final_stats, &STLB);
        L1I.replacement_final_stats = std::bind(&CACHE::lru_final_stats, &L1I);
        L1D.replacement_final_stats = std::bind(&CACHE::lru_final_stats, &L1D);
        L2C.replacement_final_stats = std::bind(&CACHE::lru_final_stats, &L2C);
    }

    // functions
    uint32_t init_instruction(ooo_model_instr instr);
    void fetch_instruction(),
         decode_and_dispatch(),
         schedule_instruction(),
         execute_instruction(),
         schedule_memory_instruction(),
         execute_memory_instruction(),
         do_scheduling(uint32_t rob_index),  
         reg_dependency(uint32_t rob_index),
         do_execution(uint32_t rob_index),
         do_memory_scheduling(uint32_t rob_index),
         operate_lsq();
    uint32_t complete_execution(uint32_t rob_index);
    void reg_RAW_dependency(uint32_t prior, uint32_t current, uint32_t source_index),
         reg_RAW_release(uint32_t rob_index),
         mem_RAW_dependency(uint32_t prior, uint32_t current, uint32_t data_index, uint32_t lq_index),
         handle_merged_translation(PACKET *provider),
         handle_merged_load(PACKET *provider),
         release_load_queue(uint32_t lq_index);

    void initialize_core();
    void add_load_queue(uint32_t rob_index, uint32_t data_index),
         add_store_queue(uint32_t rob_index, uint32_t data_index),
         execute_store(uint32_t rob_index, uint32_t sq_index, uint32_t data_index);
    int  execute_load(uint32_t rob_index, uint32_t sq_index, uint32_t data_index);
    void check_dependency(int prior, int current);
    void operate_cache();
    void update_rob();
    void retire_rob();

    uint32_t check_rob(uint64_t instr_id);

    uint32_t check_and_add_lsq(uint32_t rob_index);

    uint8_t mem_reg_dependence_resolved(uint32_t rob_index);

  // branch predictor
  uint8_t predict_branch(uint64_t ip, uint64_t predicted_target, uint8_t always_taken, uint8_t branch_type);
  void initialize_branch_predictor(),
    last_branch_result(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type);

  // btb
  std::pair<uint64_t, uint8_t> btb_prediction(uint64_t ip, uint8_t branch_type);
  void initialize_btb(),
    update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type);

  // code prefetching
  void l1i_prefetcher_initialize();
  void l1i_prefetcher_branch_operate(uint64_t ip, uint8_t branch_type, uint64_t branch_target);
  void l1i_prefetcher_cache_operate(uint64_t v_addr, uint8_t cache_hit, uint8_t prefetch_hit);
  void l1i_prefetcher_cycle_operate();
  void l1i_prefetcher_cache_fill(uint64_t v_addr, uint32_t set, uint32_t way, uint8_t prefetch, uint64_t evicted_v_addr);
  void l1i_prefetcher_final_stats();
  int prefetch_code_line(uint64_t pf_v_addr);
};

#endif

