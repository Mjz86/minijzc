// TU: the constexpr-tour battery (runtime flavor via cx_pipeline_rt()).
#include "minijz/tests/cx_tours.hpp"

// the one stable external symbol for the app layer (see cx_tours.hpp tail)
bool cx_pipeline_rt() { return cx_pipeline(); }
