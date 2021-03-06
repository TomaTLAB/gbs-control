#ifndef FRAMESYNC_H_
#define FRAMESYNC_H_

#include "debug.h"

template <class GBS, class Attrs>
class FrameSyncManager {
  private:
    typedef typename GBS::STATUS_VDS_VERT_COUNT VERT_COUNT;
    typedef typename GBS::VDS_HSYNC_RST HSYNC_RST;
    typedef typename GBS::VDS_VSYNC_RST VSYNC_RST;
    typedef typename GBS::VDS_VS_ST VSST;
    typedef typename GBS::VDS_VS_SP VSSP;
    typedef typename GBS::template Tie<VSYNC_RST, VSST, VSSP> VRST_SST_SSP;

    static const uint8_t debugInPin = Attrs::debugInPin;
    //static const uint8_t vsyncInPin = Attrs::vsyncInPin;
    static const uint32_t syncTimeout = Attrs::timeout;
    static const int16_t syncCorrection = Attrs::correction;
    static const uint32_t syncTargetPhase = Attrs::targetPhase;
    static const uint8_t syncHtotalStable = Attrs::htotalStable;
    static const uint8_t syncSamples = Attrs::samples;

    static bool syncLockReady;
    static int16_t syncLastCorrection;

    // Sample vsync start and stop times (for two consecutive frames)
    // from debug pin A timeout prevents deadlocks in case of bad
    // signals.
    // Sources can disappear while in sampling, so keep the timeout.
    static bool vsyncInputSample(unsigned long *start, unsigned long *stop) {
      unsigned long timeoutStart = micros();
      while (digitalRead(debugInPin))
        if (micros() - timeoutStart >= syncTimeout)
          return false;
      while (!digitalRead(debugInPin))
        if (micros() - timeoutStart >= syncTimeout)
          return false;
      *start = micros();
      while (digitalRead(debugInPin))
        if (micros() - timeoutStart >= syncTimeout)
          return false;
      while (!digitalRead(debugInPin))
        if (micros() - timeoutStart >= syncTimeout)
          return false;
      // sample twice
      while (digitalRead(debugInPin))
        if (micros() - timeoutStart >= syncTimeout)
          return false;
      while (!digitalRead(debugInPin))
        if (micros() - timeoutStart >= syncTimeout)
          return false;
      *stop = micros();
      return true;
    }

    // Sample vsync start and stop times from output vsync pin A
    // timeout prevents deadlocks in case of bad signals.
    // Sources can disappear while in sampling, so keep the timeout.
    static bool vsyncOutputSample(unsigned long *start, unsigned long *stop) {
      unsigned long timeoutStart = micros();
      while (digitalRead(debugInPin))
        if (micros() - timeoutStart >= syncTimeout)
          return false;
      while (!digitalRead(debugInPin))
        if (micros() - timeoutStart >= syncTimeout)
          return false;
      *start = micros();
      while (digitalRead(debugInPin))
        if (micros() - timeoutStart >= syncTimeout)
          return false;
      while (!digitalRead(debugInPin))
        if (micros() - timeoutStart >= syncTimeout)
          return false;
      *stop = micros();
      return true;
    }

    // Sample input and output vsync periods and their phase
    // difference in microseconds
    static bool vsyncPeriodAndPhase(uint32_t *periodInput,
                                    uint32_t *periodOutput, int32_t *phase) {
      unsigned long inStart, inStop, outStart, outStop, inPeriod, outPeriod,
               diff;

      GBS::TEST_BUS_SEL::write(10);
      if (!vsyncInputSample(&inStart, &inStop)) {
        return false;
      }
      inPeriod = (inStop - inStart) / 2;

      GBS::TEST_BUS_SEL::write(2);
      if (!vsyncOutputSample(&outStart, &outStop)) {
        GBS::TEST_BUS_SEL::write(10);
        return false;
      }
      GBS::TEST_BUS_SEL::write(10);
      outPeriod = outStop - outStart;
      diff = (outStart - inStart) % inPeriod;
      if (periodInput)
        *periodInput = inPeriod;
      if (periodOutput)
        *periodOutput = outPeriod;
      if (phase)
        *phase = (diff < inPeriod / 2) ? diff : diff - inPeriod;

      // good for jitter tests
      //  static uint32_t minseen = 100000, maxseen = 0;
      //  static uint8_t initialHold = 22;
      //  if (initialHold-- < 3) {
      //    if (inPeriod < minseen) minseen = inPeriod;
      //    if (inPeriod > maxseen) maxseen = inPeriod;
      //    initialHold = 2;
      //  }
      //  Serial.print("inPeriod: "); Serial.print(inPeriod);
      //  Serial.print(" min|max: "); Serial.print(minseen);
      //  Serial.print("|"); Serial.println(maxseen);

      return true;
    }

    static bool sampleVsyncPeriods(uint32_t *input, uint32_t *output) {
      uint32_t inPeriod, outPeriod;
      uint32_t inSum = 0;
      uint32_t outSum = 0;

      for (uint8_t i = 0; i < syncSamples; ++i) {
        if (!vsyncPeriodAndPhase(&inPeriod, &outPeriod, NULL))
          return false;
        inSum += inPeriod;
        outSum += outPeriod;
      }

      *input = inSum / syncSamples;
      *output = outSum / syncSamples;

      return true;
    }

    // Find the largest htotal that makes output frame time less than
    // the input.
    // todo:
    //Base htotal: 0
    //Best htotal: 0
    //Guru Meditation Error: Core  1 panic'ed (IntegerDivideByZero)
    //. Exception was unhandled.

    static bool findBestHTotal(uint16_t &bestHtotal) {
      uint16_t htotal = HSYNC_RST::read();
      uint32_t inPeriod, outPeriod;
      uint16_t candHtotal;
      uint8_t stable = 0;

      unsigned long timeout = millis();
      while ((stable < syncHtotalStable) && (millis() - timeout) < 5000) {
        yield();
        if (!sampleVsyncPeriods(&inPeriod, &outPeriod))
          return false;
        candHtotal = (htotal * inPeriod) / outPeriod;
        //Serial.print("Candidate htotal: "); Serial.println(candHtotal);

        if (candHtotal == bestHtotal)
          stable++;
        else
          stable = 1;
        bestHtotal = candHtotal;
      }

      return true;
    }

    static void adjustFrameSize(int16_t delta) {
      uint16_t vtotal = 0, vsst = 0, vssp = 0;
      uint16_t currentLineNumber, earlyFrameBoundary;
      uint16_t timeout = 10; // this routine usually finishes on first or second attempt
      VRST_SST_SSP::read(vtotal, vsst, vssp);
      earlyFrameBoundary = vtotal / 4;
      vtotal += delta;
      vsst += delta;
      vssp += delta;
      do {
        // wait for next frame start + 20 lines for stability
        currentLineNumber = GBS::STATUS_VDS_VERT_COUNT::read();
      } while ((currentLineNumber > earlyFrameBoundary || currentLineNumber < 20) && --timeout > 0);
      VRST_SST_SSP::write(vtotal, vsst, vssp);
    }

  public:
    static void reset() {
      syncLockReady = false;
      syncLastCorrection = 0;
    }

    // Initialize sync locking
    static bool init() {
      uint16_t bestHTotal = 0;

      // Adjust output horizontal sync timing so that the overall
      // frame time is as close to the input as possible while still
      // being less.  Increasing the vertical frame size slightly
      // should then push the output frame time to being larger than
      // the input.
      if (!findBestHTotal(bestHTotal)) {
        return false;
      }

      // set best Htotal and related VDS parameters
      uint16_t hTotal = HSYNC_RST::read();
      int diffHTotal = bestHTotal - hTotal;
      GBS::VDS_HB_SP::write( GBS::VDS_HB_SP::read() + diffHTotal );
      GBS::VDS_HB_ST::write( GBS::VDS_HB_ST::read() + diffHTotal );
      HSYNC_RST::write(bestHTotal);
      GBS::VDS_DIS_HB_ST::write( GBS::VDS_DIS_HB_ST::read() + diffHTotal );
      GBS::VDS_DIS_HB_SP::write( GBS::VDS_DIS_HB_SP::read() + diffHTotal );
      GBS::VDS_HS_ST::write( GBS::VDS_HS_ST::read() + diffHTotal );
      GBS::VDS_HS_SP::write( GBS::VDS_HS_SP::read() + diffHTotal );
      Serial.print("Base: "); Serial.print(hTotal);
      Serial.print(" Best: "); Serial.print(bestHTotal);
      Serial.print(" Diff: "); Serial.println(diffHTotal);

      syncLockReady = true;
      return true;
    }

    static bool ready(void) {
      return syncLockReady;
    }

    static int16_t getSyncLastCorrection() {
      return syncLastCorrection;
    }

    // Perform vsync phase locking.  This is accomplished by measuring
    // the period and phase offset of the input and output vsync
    // signals and adjusting the frame size (and thus the output vsync
    // frequency) to bring the phase offset closer to the desired
    // value.
    static bool run(void) {
      uint32_t period;
      int32_t phase;
      int32_t target;
      int16_t correction;

      if (!syncLockReady)
        return false;

      if (!vsyncPeriodAndPhase(&period, NULL, &phase))
        return false;

      //Serial.print("Phase offset: "); Serial.println(phase);

      target = (syncTargetPhase * period) / 360; // -300; //debug

      if (phase > target)
        correction = 0;
      else
        correction = syncCorrection;

      //Serial.print("Correction: "); Serial.println(correction);

      adjustFrameSize(correction - syncLastCorrection);
      syncLastCorrection = correction;

      return true;
    }
};

template <class GBS, class Attrs>
int16_t FrameSyncManager<GBS, Attrs>::syncLastCorrection;

template <class GBS, class Attrs>
bool FrameSyncManager<GBS, Attrs>::syncLockReady;
#endif
