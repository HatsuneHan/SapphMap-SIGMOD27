#pragma once

#include "external_memory/tools/enigmap/oram/pathoram/oram.hpp"
#include "external_memory/tools/enigmap/otree/otree.hpp"

namespace EnigMapCompat {
using ORAMClient_t = _ORAM::PathORAM::ORAMClient::ORAMClient<
    _OBST::Node, ORAM__Z, false, ORAM_SERVER__LEVELS_PER_PACK>;
using OramClient_t = _OBST::OramClient::OramClient<ORAMClient_t>;
using OBST_t = _OBST::OBST::OBST<OramClient_t>;
using K = _OBST::K;
using V = _OBST::V;
}
