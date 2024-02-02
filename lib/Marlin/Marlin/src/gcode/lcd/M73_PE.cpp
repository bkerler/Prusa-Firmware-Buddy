#include "../../inc/MarlinConfig.h"

#include "../gcode.h"
#include "../../lcd/ultralcd.h"
#include "../../sd/cardreader.h"

#include "M73_PE.h"
#include "../Marlin/src/libs/stopwatch.h"
extern Stopwatch print_job_timer;

ClProgressData oProgressData;

void ClValidityValue::mInit(void) {
    // nTime=0;
    bIsUsed = false;
}

void ClValidityValue::mSetValue(uint32_t nN, uint32_t nNow) {
    nValue = nN;
    nTime = nNow;
    bIsUsed = true;
}

uint32_t ClValidityValue::mGetValue(void) {
    return (nValue);
}

bool ClValidityValue::mIsActual(uint32_t nNow) {
    return (mIsActual(nNow, PROGRESS_DATA_VALIDITY_PERIOD));
}

bool ClValidityValue::mIsActual(uint32_t nNow, uint16_t nPeriod) {
    // return((nTime+nPeriod)>=nNow);
    return (mIsUsed() && ((nTime + nPeriod) >= nNow));
}

bool ClValidityValue::mIsUsed(void) {
    // return(nTime>0);
    return (bIsUsed);
}

void ClValidityValueSec::mFormatSeconds(char *sStr, uint16_t nFeedrate) {
    uint8_t nDay, nHour, nMin;
    uint32_t nRest;

    nRest = (nValue * 100) / nFeedrate;
    nDay = nRest / (60 * 60 * 24);
    nRest = nRest % (60 * 60 * 24);
    nHour = nRest / (60 * 60);
    nRest = nRest % (60 * 60);
    nMin = nRest / 60;
    if (nDay > 0) {
        sprintf_P(sStr, PSTR("%dd %dh"), nDay, nHour);
    } else if (nHour > 0) {
        sprintf_P(sStr, PSTR("%dh %dm"), nHour, nMin);
    } else {
        sprintf_P(sStr, PSTR("%dm"), nMin);
    }
}

void ClProgressData::mInit(void) {
    oPercentDirectControl.mInit();
    oPercentDone.mInit();
    oTime2End.mInit();
    oTime2Pause.mInit();
}

#if ENABLED(M73_PRUSA)

/** \addtogroup G-Codes
 * @{
 */

/**
 * M73: Tell the firmware the current build progress (percentage/time to end/time to pause). The machine is expected to display this on its display.
 *
 * ## Parameters
 *
 * - `P` - [percentage] Set percentage value
 * - `R` - [minutes] Set time to end / percentage done
 * - `C` - [minutes] Set time to pause
 *   (historically also `T`, which is not documented anywhere and we couldn't
 *   track anyone producing it, but the code accepted it previously - probably
 *   a typo, but keeping it for backwards compatibility anyway).
 */

void GcodeSuite::M73_PE() {
    std::optional<uint8_t> P = std::nullopt;
    std::optional<uint32_t> R = std::nullopt;
    std::optional<uint32_t> C = std::nullopt;
    if (parser.seen('P')) {
        P = parser.value_byte();
    }
    if (parser.seen('R')) {
        R = parser.value_ulong() * 60;
    }
    if (parser.seen('T')) {
        C = parser.value_ulong() * 60;
    }
    if (parser.seen('C')) {
        C = parser.value_ulong() * 60;
    }

    M73_PE_no_parser(P, R, C);
}

/** @}*/

void M73_PE_no_parser(std::optional<uint8_t> P, std::optional<uint32_t> R, std::optional<uint32_t> C) {
    uint32_t nTimeNow;
    uint8_t nValue;

    // ui.set_progress_time(...);
    nTimeNow = print_job_timer.duration(); // !!! [s]
    if (P) {
        nValue = *P;
        if (R) {
            oProgressData.oPercentDone.mSetValue((uint32_t)nValue, nTimeNow);
            oProgressData.oTime2End.mSetValue(*R, nTimeNow); // [min] -> [s]
        } else {
            oProgressData.oPercentDirectControl.mSetValue((uint32_t)nValue, nTimeNow);
        }
    }

    if (C) {
        if (*C == 0) {
            // Pause happening now, reset the countdown (there doesn't have to be another until the end).
            oProgressData.oTime2Pause.mInit();
        } else {
            oProgressData.oTime2Pause.mSetValue(*C, nTimeNow); // [min] -> [s]
        }
    }
}

#endif
