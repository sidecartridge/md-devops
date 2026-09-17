#include "tprotocol.h"

uint32_t tprotocol_last_header_found = 0;
uint32_t tprotocol_new_header_found = 0;
TPParseStep tprotocol_nextTPstep = HEADER_DETECTION;
TransmissionProtocol tprotocol_transmission = {0};
// EPIC-18 STORY-01 instrumentation: how often the idle-gap rule had to drag
// the parser back to HEADER_DETECTION, and how often it did so from a frame
// that was already in progress (the interesting case).
uint32_t tprotocol_resyncs = 0;
uint32_t tprotocol_resyncsMidFrame = 0;
