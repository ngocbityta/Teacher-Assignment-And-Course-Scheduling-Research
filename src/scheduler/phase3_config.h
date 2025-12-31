#pragma once

namespace phase3 {
namespace config {

// ============================================================================
// Objective Function Weights
// ============================================================================
constexpr double WEIGHT_COURSE_PREF = 1.0;
constexpr double WEIGHT_TIME_PREF = 1.0;
constexpr double WEIGHT_WORKLOAD_BALANCE = 5.0;
constexpr double WEIGHT_COMPACTNESS = 3.0;
constexpr double WEIGHT_STABILITY = 2.0;
constexpr double WEIGHT_ROOM_PENALTY = 5.0;

// ============================================================================
// Stability Penalties
// ============================================================================
constexpr double STABILITY_TEACHER_PENALTY = 1.5;
constexpr double STABILITY_TIMESLOT_PENALTY = 1.0;
constexpr double STABILITY_CORE_MULTIPLIER = 1.5;

// ============================================================================
// Simulated Annealing Parameters
// ============================================================================
constexpr double INITIAL_TEMPERATURE = 1.0;
constexpr double COOLING_RATE = 0.98;
constexpr double MIN_TEMPERATURE = 1e-4;
constexpr int MAX_ITERATIONS = 1200;
constexpr int BASE_MOVES_PER_NEIGHBORHOOD = 32;

// ============================================================================
// Search Control Parameters
// ============================================================================
constexpr int LIMIT_NO_IMPROVEMENT = 220;
constexpr int STUCK_THRESHOLD = 100;
constexpr int INTENSIFICATION_INTERVAL = 50;

// ============================================================================
// Tabu Search Parameters
// ============================================================================
constexpr int DEFAULT_TABU_TENURE = 50;
constexpr double BEST_TOLERANCE = 0.01;

// ============================================================================
// Neighborhood Selection Parameters
// ============================================================================
constexpr int NUM_NEIGHBORHOODS = 5;
constexpr double EMA_BETA = 0.1;
constexpr double WEIGHT_MIN = 0.2;
constexpr double WEIGHT_MAX = 3.0;

// Initial neighborhood scores (normalized to [0,1])
constexpr double INIT_SCORE_SINGLE_CHANGE = 1.2;
constexpr double INIT_SCORE_BLOCK_RELOCATE = 0.8;
constexpr double INIT_SCORE_CHAIN_MOVE = 0.5;
constexpr double INIT_SCORE_ROOM_MOVES = 0.7;
constexpr double INIT_SCORE_MULTI_SWAP = 0.6;

// ============================================================================
// Restart and Reheating Parameters
// ============================================================================
constexpr int MAX_REHEATS = 1;
constexpr int MAX_RESTARTS = 2;
constexpr double REHEAT_FACTOR = 0.3;
constexpr double RESTART_TEMP_FACTOR = 0.3;
constexpr int REHEAT_BUFFER = 50;

// Local search shake parameters
constexpr int MIN_LOCAL_SHAKES = 4;
constexpr int MAX_LOCAL_SHAKES = 40;
constexpr double TEMP_THRESHOLD_FOR_SHAKES = 0.1;

// ============================================================================
// Numerical Stability Parameters
// ============================================================================
constexpr double MAX_EXPONENT = 700.0;
constexpr double MIN_EXPONENT = -700.0;

// ============================================================================
// Performance Tuning
// ============================================================================
constexpr int MAX_PERIOD_INDEX = 128;

// ============================================================================
// Adaptive Parameters
// ============================================================================
constexpr double NEIGHBORHOOD_DECAY_RATE = 0.9995;
constexpr double STUCK_DECAY_FACTOR = 0.995;

} // namespace config
} // namespace phase3
