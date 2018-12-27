#pragma once

#include <cstdint>

class Consensus {
public:
  /** @brief   Set the flag to log solver-with-state messages to console*/
  constexpr static bool Log = true;

  /** @brief   True if re-select write node on timeout is enabled*/
  constexpr static bool ReSelectWriteOnTimeout = false;

  /** @brief   True if write node may to reduce desired count of hashes on big bang and spawn next round immediately*/
  constexpr static bool ReduceMinHashesOnBigBang = false;

  /** @brief   The default state timeout */
  constexpr static unsigned int DefaultStateTimeout = 5000;

  /** @brief   Maximum time in msec to wait new round after consensus achieved, after that waiting trusted nodes
   * activates */
  constexpr static unsigned int PostConsensusTimeout = 60000;

  /** @brief   The minimum trusted nodes to start consensus */
  constexpr static unsigned int MinTrustedNodes = 3;

  /** @brief   The maximum trusted nodes to take part in consensus */
  constexpr static unsigned int MaxTrustedNodes = 5;

  /** @brief   The return value means: general (Writer->General) is not selected by "generals" */
  constexpr static uint8_t GeneralNotSelected = 100;

  /** @brief   Max duration (msec) of the whole round (SolverCore on the 1st round) */
  constexpr static uint32_t T_round = 2000;

  /** @brief   Max timeout (msec) to wait stages (Trusted-2,3) */
  constexpr static uint32_t T_stage_request = 4000;

  /** @brief   Max timeout (msec) to force further transition (Trusted-2) */
  // constexpr static uint32_t T_stage_force = 5000;

  /** @brief   Max time to collect transactions (PermanentWrite, SolverCore on BigBang) */
  constexpr static uint32_t T_coll_trans = 500;
};
