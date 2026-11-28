// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"
#include "ow_slave.h"

struct BusStats {
  ow::Counters protocol;
  uint32_t lateReactions = 0;
  uint32_t maxReactionCycles = 0;
  uint32_t firstEdges = 0;
  uint32_t overruns = 0;
  uint32_t stuckLow = 0;
  uint32_t logSequence = 0;
  uint32_t realErrors = 0;
  bool pendingConfig = false;
};

esp_err_t slaveBusStart(const ow::Config& config);
void slaveBusQueueConfig(const ow::Config& config);
void slaveBusRealTemperature(int16_t temperature);
void slaveBusRealFailure();
bool slaveBusRealReading(int16_t* temperature);
BusStats slaveBusStats();
size_t slaveBusLogs(ow::Transaction* out, size_t capacity);
