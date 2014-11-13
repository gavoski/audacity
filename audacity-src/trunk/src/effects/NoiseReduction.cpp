/**********************************************************************

  Audacity: A Digital Audio Editor

  NoiseReduction.cpp

  Dominic Mazzoni

  detailed rewriting by
  Paul Licameli

*******************************************************************//**

\class EffectNoiseReduction
\brief A two-pass effect to reduce background noise.

  The first pass is done over just noise.  For each windowed sample
  of the sound, we take a FFT and then statistics are tabulated for
  each frequency band.

  During the noise reduction phase, we start by setting a gain control
  for each frequency band such that if the sound has exceeded the
  previously-determined threshold, the gain is set to 0 dB, otherwise
  the gain is set lower (e.g. -18 dB), to suppress the noise.
  Then time-smoothing is applied so that the gain for each frequency
  band moves slowly, and then frequency-smoothing is applied so that a
  single frequency is never suppressed or boosted in isolation.
  Lookahead is employed; this effect is not designed for real-time
  but if it were, there would be a significant delay.

  The gain controls are applied to the complex FFT of the signal,
  and then the inverse FFT is applied.  A Hanning window may be
  applied (depending on the advanced window types setting), and then
  the output signal is then pieced together using overlap/add.

*//****************************************************************//**
*/

#include "../Audacity.h"
#include "../Experimental.h"
#include "NoiseReduction.h"

#include "../Prefs.h"

#include <algorithm>
#include <vector>
#include <math.h>

#if defined(__WXMSW__) && !defined(__CYGWIN__)
#include <float.h>
#define finite(x) _finite(x)
#endif

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/radiobut.h>
#include <wx/slider.h>
#include <wx/valtext.h>
#include <wx/textctrl.h>

typedef std::vector<float> FloatVector;

// Define to make the radio button three-way
#define RESIDUE_CHOICE

// Define to expose other advanced, experimental dialog controls
#define ADVANCED_SETTINGS

// Define to make the old statistical methods an available choice
#define OLD_METHOD_AVAILABLE

namespace {

enum DiscriminationMethod {
   DM_MEDIAN,
   DM_SECOND_GREATEST,
   DM_OLD_METHOD,

   DM_N_METHODS,
   DM_DEFAULT_METHOD = DM_SECOND_GREATEST,
};

const struct DiscriminationMethodInfo {
   const wxChar *name;
} discriminationMethodInfo[DM_N_METHODS] = {
      { _("Median") },
      { _("Second greatest") },
      { _("Old") },
};

// magic number used only in the old statistics
// and the old discrimination
const float minSignalTime = 0.05f;

enum WindowTypes {
   WT_RECTANGULAR_HANN = 0, // 2.0.6 behavior, requires 1/2 step
   WT_HANN_RECTANGULAR, // requires 1/2 step
   WT_HANN_HANN,        // requires 1/4 step
   WT_BLACKMAN_HANN,     // requires 1/4 step

   WT_N_WINDOW_TYPES,
   WT_DEFAULT_WINDOW_TYPES = WT_HANN_HANN
};

const struct WindowTypesInfo {
   const wxChar *name;
   int minSteps;
   double inCoefficients[3];
   double outCoefficients[3];
   double productConstantTerm;
} windowTypesInfo [WT_N_WINDOW_TYPES] = {
   // In all of these cases, the constant term of the product of windows
   // is the product of the windows' two constant terms,
   // plus one half the product of the first cosine coefficients.

   { _("none, Hann (2.0.6 behavior)"),    2, { 1, 0, 0 },            { 0.5, -0.5, 0 }, 0.5 },
   { _("Hann, none"),                     2, { 0.5, -0.5, 0 },       { 1, 0, 0 },      0.5 },
   { _("Hann, Hann (default)"),           4, { 0.5, -0.5, 0 },       { 0.5, -0.5, 0 }, 0.375 },
   { _("Blackman, Hann"),                 4, { 0.42, -0.5, 0.08 },   { 0.5, -0.5, 0 }, 0.335 },
};

enum {
   DEFAULT_WINDOW_SIZE_CHOICE = 8, // corresponds to 2048
   DEFAULT_STEPS_PER_WINDOW_CHOICE = 1 // corresponds to 4, minimum for WT_HANN_HANN
};

enum  NoiseReductionChoice {
   NRC_REDUCE_NOISE,
   NRC_ISOLATE_NOISE,
   NRC_LEAVE_RESIDUE,
};

} // namespace

//----------------------------------------------------------------------------
// EffectNoiseReduction::Statistics
//----------------------------------------------------------------------------

class EffectNoiseReduction::Statistics
{
public:
   Statistics(int spectrumSize, double rate, int windowTypes)
      : mRate(rate)
      , mWindowSize((spectrumSize - 1) * 2)
      , mWindowTypes(windowTypes)
      , mTotalWindows(0)
      , mTrackWindows(0)
      , mSums(spectrumSize)
      , mMeans(spectrumSize)
#ifdef OLD_METHOD_AVAILABLE
      , mNoiseThreshold(spectrumSize)
#endif
   {}

   // Noise profile statistics follow

   double mRate; // Rate of profile track(s) -- processed tracks must match
   int mWindowSize;
   int mWindowTypes;

   int mTotalWindows;
   int mTrackWindows;
   FloatVector mSums;
   FloatVector mMeans;

#ifdef OLD_METHOD_AVAILABLE
   // Old statistics:
   FloatVector mNoiseThreshold;
#endif
};

//----------------------------------------------------------------------------
// EffectNoiseReduction::Settings
//----------------------------------------------------------------------------

// This object is the memory of the effect between uses
// (other than noise profile statistics)
class EffectNoiseReduction::Settings
{
public:
   Settings();
   ~Settings() {}

   bool PromptUser(EffectNoiseReduction *effect,
      wxWindow *parent, bool bHasProfile, bool bAllowTwiddleSettings);
   bool PrefsIO(bool read);
   bool Validate() const;

   int WindowSize() const { return 1 << (3 + mWindowSizeChoice); }
   int StepsPerWindow() const { return 1 << (1+ mStepsPerWindowChoice); }

   bool      mDoProfile;

   // Stored in preferences:

   // Basic:
   double     mSensitivity;       // in dB, plus or minus
   double     mFreqSmoothingHz;
   double     mNoiseGain;         // in dB, positive
   double     mAttackTime;        // in secs
   double     mReleaseTime;       // in secs

   // Advanced:
   double     mNewSensitivity;   // - log10 of a probability... yeah.

   // Basic:
   int        mNoiseReductionChoice;

   // Advanced:
   int        mWindowTypes;
   int        mWindowSizeChoice;
   int        mStepsPerWindowChoice;
   int        mMethod;
};

EffectNoiseReduction::Settings::Settings()
   : mDoProfile(true)
{
   PrefsIO(true);
}

//----------------------------------------------------------------------------
// EffectNoiseReduction::Worker
//----------------------------------------------------------------------------

// This object holds information needed only during effect calculation
class EffectNoiseReduction::Worker
{
public:
   typedef EffectNoiseReduction::Settings Settings;
   typedef  EffectNoiseReduction::Statistics Statistics;

   Worker(const Settings &settings, double sampleRate
#ifdef EXPERIMENTAL_SPECTRAL_EDITING
          , double f0, double f1
#endif
      );
   ~Worker();

   bool Process(EffectNoiseReduction &effect,
                Statistics &statistics, TrackFactory &factory,
                SelectedTrackListOfKindIterator &iter, double mT0, double mT1);

private:
   bool ProcessOne(EffectNoiseReduction &effect,
                   Statistics &statistics,
                   TrackFactory &factory,
                   int count, WaveTrack *track,
                   sampleCount start, sampleCount len);

   void StartNewTrack();
   void ProcessSamples(Statistics &statistics,
      WaveTrack *outputTrack, sampleCount len, float *buffer);
   void FillFirstHistoryWindow();
   void ApplyFreqSmoothing(FloatVector &gains);
   void GatherStatistics(Statistics &statistics);
   inline bool Classify(const Statistics &statistics, int band);
   void ReduceNoise(const Statistics &statistics, WaveTrack *outputTrack);
   void RotateHistoryWindows();
   void FinishTrackStatistics(Statistics &statistics);
   void FinishTrack(Statistics &statistics, WaveTrack *outputTrack);

private:

   const bool mDoProfile;

   const double mSampleRate;

   const int mWindowSize;
   // These have that size:
   HFFT     hFFT;
   FloatVector mFFTBuffer;
   FloatVector mInWaveBuffer;
   FloatVector mOutOverlapBuffer;
   // These have that size, or 0:
   FloatVector mInWindow;
   FloatVector mOutWindow;

   const int mSpectrumSize;
   FloatVector mFreqSmoothingScratch;
   const int mFreqSmoothingBins;
   // When spectral selection limits the affected band:
   int mBinLow;  // inclusive lower bound
   int mBinHigh; // exclusive upper bound

   const int mNoiseReductionChoice;
   const int mStepsPerWindow;
   const int mStepSize;
   const int mMethod;
   const double mNewSensitivity;


   sampleCount       mInSampleCount;
   sampleCount       mOutStepCount;
   int                   mInWavePos;

   float     mOneBlockAttack;
   float     mOneBlockRelease;
   float     mNoiseAttenFactor;
   float     mSensitivityFactor;

   int       mNWindowsToExamine;
   int       mCenter;
   int       mHistoryLen;

   struct Record
   {
      Record(int spectrumSize)
         : mSpectrums(spectrumSize)
         , mGains(spectrumSize)
         , mRealFFTs(spectrumSize - 1)
         , mImagFFTs(spectrumSize - 1)
      {
      }

      FloatVector mSpectrums;
      FloatVector mGains;
      FloatVector mRealFFTs;
      FloatVector mImagFFTs;
   };
   std::vector<Record*> mQueue;

   friend EffectNoiseReduction::Dialog;
};

/****************************************************************//**

\class EffectNoiseReduction::Dialog
\brief Dialog used with EffectNoiseReduction

//*******************************************************************/

//----------------------------------------------------------------------------
// EffectNoiseReduction::Dialog
//----------------------------------------------------------------------------

class EffectNoiseReduction::Dialog: public EffectDialog
{
public:
   // constructors and destructors
   Dialog
      (EffectNoiseReduction *effect,
       Settings *settings,
       wxWindow *parent, bool bHasProfile,
       bool bAllowTwiddleSettings);

   void PopulateOrExchange(ShuttleGui & S);
   bool TransferDataToWindow();
   bool TransferDataFromWindow();

   const Settings &GetTempSettings() const
   { return mTempSettings; }

private:
   void DisableControlsIfIsolating();

#ifdef ADVANCED_SETTINGS
   void EnableSensitivityControls();
#endif

   // handlers
   void OnGetProfile( wxCommandEvent &event );
   void OnNoiseReductionChoice( wxCommandEvent &event );
#ifdef ADVANCED_SETTINGS
   void OnMethodChoice(wxCommandEvent &);
#endif
   void OnPreview(wxCommandEvent &event);
   void OnReduceNoise( wxCommandEvent &event );
   void OnCancel( wxCommandEvent &event );

   void OnText(wxCommandEvent &event);
   void OnSlider(wxCommandEvent &event);

   // data members

   EffectNoiseReduction *m_pEffect;
   EffectNoiseReduction::Settings *m_pSettings;
   EffectNoiseReduction::Settings mTempSettings;

   bool mbHasProfile;
   bool mbAllowTwiddleSettings;


   wxRadioButton *mKeepSignal;
   wxRadioButton *mKeepNoise;
#ifdef RESIDUE_CHOICE
   wxRadioButton *mResidue;
#endif

private:
    DECLARE_EVENT_TABLE()
};

EffectNoiseReduction::EffectNoiseReduction()
: mSettings(new EffectNoiseReduction::Settings)
{
   Init();
}

EffectNoiseReduction::~EffectNoiseReduction()
{
}

bool EffectNoiseReduction::Init()
{
   return true;
}

bool EffectNoiseReduction::CheckWhetherSkipEffect()
{
   return false;
}

wxString EffectNoiseReduction::GetEffectName()
{
   return wxString(_("Noise Reduction..."));
}

std::set<wxString> EffectNoiseReduction::GetEffectCategories()
{
   std::set<wxString> result;
   result.insert(wxT("http://audacityteam.org/namespace#NoiseReduction"));
   return result;
}

wxString EffectNoiseReduction::GetEffectIdentifier()
{
   return wxString(wxT("NoiseReduction"));
}

wxString EffectNoiseReduction::GetEffectAction()
{
  if (mSettings->mDoProfile)
     return wxString(_("Creating Noise Profile"));
  else
     return wxString(_("Reducing Noise"));
}

bool EffectNoiseReduction::PromptUser()
{
   // We may want to twiddle the levels if we are setting
   // from an automation dialog, the only case in which we can
   // get here without any wavetracks.
   return mSettings->PromptUser(this, mParent,
      (mStatistics.get() != 0), (GetNumWaveTracks() == 0));
}

bool EffectNoiseReduction::Settings::PromptUser
(EffectNoiseReduction *effect, wxWindow *parent,
 bool bHasProfile, bool bAllowTwiddleSettings)
{
   EffectNoiseReduction::Dialog dlog
      (effect, this, parent, bHasProfile, bAllowTwiddleSettings);

   dlog.CentreOnParent();
   dlog.ShowModal();

   if (dlog.GetReturnCode() == 0)
      return false;

   *this = dlog.GetTempSettings();
   mDoProfile = (dlog.GetReturnCode() == 1);

   return PrefsIO(false);
}

namespace {
   template <typename StructureType, typename FieldType>
   struct PrefsTableEntry {
      typedef FieldType (StructureType::*MemberPointer);

      MemberPointer field;
      const wxChar *name;
      FieldType defaultValue;
   };

   template <typename StructureType, typename FieldType>
   void readPrefs(
      StructureType *structure, const wxString &prefix,
      const PrefsTableEntry<StructureType, FieldType> *fields, int numFields)
   {
      for (int ii = 0; ii < numFields; ++ii) {
         const PrefsTableEntry<StructureType, FieldType> &entry = fields[ii];
         gPrefs->Read(prefix + entry.name, &(structure->*(entry.field)),
            entry.defaultValue);
      }
   }

   template <typename StructureType, typename FieldType>
   void writePrefs(
      StructureType *structure, const wxString &prefix,
      const PrefsTableEntry<StructureType, FieldType> *fields, int numFields)
   {
      for (int ii = 0; ii < numFields; ++ii) {
         const PrefsTableEntry<StructureType, FieldType> &entry = fields[ii];
         gPrefs->Write(prefix + entry.name, structure->*(entry.field));
      }
   }
}

bool EffectNoiseReduction::Settings::PrefsIO(bool read)
{
   static const double DEFAULT_SENSITIVITY = 6.0;

   static const PrefsTableEntry<Settings, double> doubleTable[] = {
         { &Settings::mSensitivity, wxT("Sensitivity"), 0.0 },
         { &Settings::mFreqSmoothingHz, wxT("FreqSmoothing"), 150.0 },
         { &Settings::mNoiseGain, wxT("Gain"), 24.0 },
         { &Settings::mAttackTime, wxT("AttackTime"), 0.15 },
         { &Settings::mReleaseTime, wxT("ReleaseTime"), 0.15 },

         // Advanced settings
         { &Settings::mNewSensitivity, wxT("NewSensitivity"), DEFAULT_SENSITIVITY },
   };
   static int doubleTableSize = sizeof(doubleTable) / sizeof(doubleTable[0]);

   static const PrefsTableEntry<Settings, int> intTable[] = {
         { &Settings::mNoiseReductionChoice, wxT("ReductionChoice"), NRC_REDUCE_NOISE },

         // Advanced settings
         { &Settings::mWindowTypes, wxT("WindowTypes"), WT_DEFAULT_WINDOW_TYPES },
         { &Settings::mWindowSizeChoice, wxT("WindowSize"), DEFAULT_WINDOW_SIZE_CHOICE },
         { &Settings::mStepsPerWindowChoice, wxT("StepsPerWindow"), DEFAULT_STEPS_PER_WINDOW_CHOICE },
         { &Settings::mMethod, wxT("Method"), DM_DEFAULT_METHOD },
   };
   static int intTableSize = sizeof(intTable) / sizeof(intTable[0]);

   static const wxString prefix(wxT("/Effects/NoiseReduction/"));

   if (read) {
      readPrefs(this, prefix, doubleTable, doubleTableSize);
      readPrefs(this, prefix, intTable, intTableSize);

      // Ignore preferences for unavailable options.
#ifndef RESIDUE_CHOICE
      if (mNoiseReductionChoice == NRC_LEAVE_RESIDUE)
         mNoiseReductionChoice = NRC_ISOLATE_NOISE;
#endif

#ifndef ADVANCED_SETTINGS
      mWindowTypes = WT_HANN_HANN;
      mWindowSizeChoice = DEFAULT_WINDOW_SIZE_CHOICE;
      mStepsPerWindowChoice = DEFAULT_STEPS_PER_WINDOW_CHOICE;
      mMethod = DM_DEFAULT_METHOD;
      mNewSensitivity = DEFAULT_SENSITIVITY;
#endif

#ifndef OLD_METHOD_AVAILABLE
      if (mMethod == DM_OLD_METHOD)
         mMethod = DM_DEFAULT_METHOD;
#endif

      return true;
   }
   else {
      writePrefs(this, prefix, doubleTable, doubleTableSize);
      writePrefs(this, prefix, intTable, intTableSize);
      return gPrefs->Flush();
   }
}

bool EffectNoiseReduction::Settings::Validate() const
{
   if (StepsPerWindow() < windowTypesInfo[mWindowTypes].minSteps) {
      ::wxMessageBox(_("Steps per block are too few for the window types."));
      return false;
   }

   if (StepsPerWindow() > WindowSize()) {
      ::wxMessageBox(_("Steps per block cannot exceed the window size."));
      return false;
   }

   if (mMethod == DM_MEDIAN && StepsPerWindow() > 4) {
      ::wxMessageBox(_("Median method is not implemented for more than four steps per window."));
      return false;
   }

   return true;
}

bool EffectNoiseReduction::TransferParameters( Shuttle & WXUNUSED(shuttle) )
{
   //shuttle.TransferDouble(wxT("Gain"), mNoiseGain, 0.0);
   //shuttle.TransferDouble(wxT("Freq"), mFreqSmoothingHz, 0.0);
   return true;
}

bool EffectNoiseReduction::Process()
{
   // This same code will either reduce noise or profile it

   this->CopyInputTracks(); // Set up mOutputTracks.

   SelectedTrackListOfKindIterator iter(Track::Wave, mOutputTracks);
   WaveTrack *track = (WaveTrack *) iter.First();
   if (!track)
      return false;

   // Initialize statistics if gathering them, or check for mismatched (advanced)
   // settings if reducing noise.
   if (mSettings->mDoProfile) {
      int spectrumSize = 1 + mSettings->WindowSize() / 2;
      mStatistics.reset
         (new Statistics(spectrumSize, track->GetRate(), mSettings->mWindowTypes));
   }
   else if (mStatistics->mWindowSize != mSettings->WindowSize()) {
      // possible only with advanced settings
      ::wxMessageBox(_("You must specify the same window size for steps 1 and 2."));
      return false;
   }
   else if (mStatistics->mWindowTypes != mSettings->mWindowTypes) {
      // A warning only
      ::wxMessageBox(_("Warning: window types are not the same as for profiling."));
   }

   Worker worker(*mSettings, mStatistics->mRate
#ifdef EXPERIMENTAL_SPECTRAL_EDITING
                 , mF0, mF1
#endif
      );
   bool bGoodResult = worker.Process(*this, *mStatistics, *mFactory, iter, mT0, mT1);
   if (mSettings->mDoProfile) {
      if (bGoodResult)
         mSettings->mDoProfile = false; // So that "repeat last effect" will reduce noise
      else
         mStatistics.release(); // So that profiling must be done again before noise reduction
   }
   this->ReplaceProcessedTracks(bGoodResult);
   return bGoodResult;
}

EffectNoiseReduction::Worker::~Worker()
{
   EndFFT(hFFT);
   for(int ii = 0, nn = mQueue.size(); ii < nn; ++ii)
      delete mQueue[ii];
}

bool EffectNoiseReduction::Worker::Process
(EffectNoiseReduction &effect, Statistics &statistics, TrackFactory &factory,
 SelectedTrackListOfKindIterator &iter, double mT0, double mT1)
{
   int count = 0;
   WaveTrack *track = (WaveTrack *) iter.First();
   while (track) {
      if (track->GetRate() != mSampleRate) {
         if (mDoProfile)
            ::wxMessageBox(_("All noise profile data must have the same sample rate."));
         else
            ::wxMessageBox(_("The sample rate of the noise profile must match that of the sound to be processed."));
         return false;
      }

      double trackStart = track->GetStartTime();
      double trackEnd = track->GetEndTime();
      double t0 = std::max(trackStart, mT0);
      double t1 = std::min(trackEnd, mT1);

      if (t1 > t0) {
         sampleCount start = track->TimeToLongSamples(t0);
         sampleCount end = track->TimeToLongSamples(t1);
         sampleCount len = (sampleCount)(end - start);

         if (!ProcessOne(effect, statistics, factory,
                         count, track, start, len))
            return false;
      }
      track = (WaveTrack *) iter.Next();
      ++count;
   }

   if (mDoProfile) {
      if (statistics.mTotalWindows == 0) {
         ::wxMessageBox(_("Selected noise profile is too short."));
         return false;
      }
   }

   return true;
}

void EffectNoiseReduction::Worker::ApplyFreqSmoothing(FloatVector &gains)
{
   // Given an array of gain mutipliers, average them
   // GEOMETRICALLY.  Don't multiply and take nth root --
   // that may quickly cause underflows.  Instead, average the logs.

   if (mFreqSmoothingBins == 0)
      return;

   {
      float *pScratch = &mFreqSmoothingScratch[0];
      std::fill(pScratch, pScratch + mSpectrumSize, 0.0f);
   }

   for (int ii = 0; ii < mSpectrumSize; ++ii)
      gains[ii] = log(gains[ii]);

   for (int ii = 0; ii < mSpectrumSize; ++ii) {
      const int j0 = std::max(0, ii - mFreqSmoothingBins);
      const int j1 = std::min(mSpectrumSize - 1, ii + mFreqSmoothingBins);
      for(int jj = j0; jj <= j1; ++jj) {
         mFreqSmoothingScratch[ii] += gains[jj];
      }
      mFreqSmoothingScratch[ii] /= (j1 - j0 + 1);
   }

   for (int ii = 0; ii < mSpectrumSize; ++ii)
      gains[ii] = exp(mFreqSmoothingScratch[ii]);
}

EffectNoiseReduction::Worker::Worker
(const Settings &settings, double sampleRate
#ifdef EXPERIMENTAL_SPECTRAL_EDITING
, double f0, double f1
#endif
)
: mDoProfile(settings.mDoProfile)

, mSampleRate(sampleRate)

, mWindowSize(settings.WindowSize())
, hFFT(InitializeFFT(mWindowSize))
, mFFTBuffer(mWindowSize)
, mInWaveBuffer(mWindowSize)
, mOutOverlapBuffer(mWindowSize)
, mInWindow()
, mOutWindow()

, mSpectrumSize(1 + mWindowSize / 2)
, mFreqSmoothingScratch(mSpectrumSize)
, mFreqSmoothingBins((int)(settings.mFreqSmoothingHz * mWindowSize / mSampleRate))
, mBinLow(0)
, mBinHigh(mSpectrumSize)

, mNoiseReductionChoice(settings.mNoiseReductionChoice)
, mStepsPerWindow(settings.StepsPerWindow())
, mStepSize(mWindowSize / mStepsPerWindow)
, mMethod(settings.mMethod)

// Sensitivity setting is a base 10 log, turn it into a natural log
, mNewSensitivity(settings.mNewSensitivity * log(10.0))

, mInSampleCount(0)
, mOutStepCount(0)
, mInWavePos(0)
{
#ifdef EXPERIMENTAL_SPECTRAL_EDITING
   {
      const double bin = mSampleRate / mWindowSize;
      if (f0 >= 0.0 )
         mBinLow = floor(f0 / bin);
      if (f1 >= 0.0)
         mBinHigh = ceil(f1 / bin);
   }
#endif

   const double noiseGain = -settings.mNoiseGain;
   const int nAttackBlocks = 1 + (int)(settings.mAttackTime * sampleRate / mStepSize);
   const int nReleaseBlocks = 1 + (int)(settings.mReleaseTime * sampleRate / mStepSize);
   // Applies to amplitudes, divide by 20:
   mNoiseAttenFactor = pow(10.0, noiseGain / 20.0);
   // Apply to gain factors which apply to amplitudes, divide by 20:
   mOneBlockAttack = pow(10.0, (noiseGain / (20.0 * nAttackBlocks)));
   mOneBlockRelease = pow(10.0, (noiseGain / (20.0 * nReleaseBlocks)));
   // Applies to power, divide by 10:
   mSensitivityFactor = pow(10.0, settings.mSensitivity / 10.0);

   mNWindowsToExamine = (mMethod == DM_OLD_METHOD)
      ? std::max(2, (int)(minSignalTime * sampleRate / mStepSize))
      : 1 + mStepsPerWindow;

   mCenter = mNWindowsToExamine / 2;
   wxASSERT(mCenter >= 1); // release depends on this assumption

   if (mDoProfile)
#ifdef OLD_METHOD_AVAILABLE
      mHistoryLen = mNWindowsToExamine;
#else
      mHistoryLen = 1;
#endif
   else {
      // Allow long enough queue for sufficient inspection of the middle
      // and for attack processing
      // See ReduceNoise()
      mHistoryLen = std::max(mNWindowsToExamine, mCenter + nAttackBlocks);
   }

   mQueue.resize(mHistoryLen);
   for (int ii = 0; ii < mHistoryLen; ++ii)
      mQueue[ii] = new Record(mSpectrumSize);

   // Create windows

   const double constantTerm =
      windowTypesInfo[settings.mWindowTypes].productConstantTerm;

   // One or the other window must by multiplied by this to correct for
   // overlap.  Must scale down as steps get smaller, and overlaps larger.
   const double multiplier = 1.0 / (constantTerm * mStepsPerWindow);

   // Create the analysis window
   switch (settings.mWindowTypes) {
   case WT_RECTANGULAR_HANN:
      break;
   default:
      {
         const double m =
            settings.mWindowTypes == WT_HANN_RECTANGULAR ? multiplier : 1;
         const double *const coefficients =
            windowTypesInfo[settings.mWindowTypes].inCoefficients;
         const double c0 = coefficients[0];
         const double c1 = coefficients[1];
         const double c2 = coefficients[2];
         mInWindow.resize(mWindowSize);
         for (int ii = 0; ii < mWindowSize; ++ii)
            mInWindow[ii] = m *
            (c0 + c1 * cos((2.0*M_PI*ii) / mWindowSize)
                + c2 * cos((4.0*M_PI*ii) / mWindowSize));
      }
      break;
   }

   if (!mDoProfile) {
      // Create the synthesis window
      switch (settings.mWindowTypes) {
      case WT_HANN_RECTANGULAR:
         break;
      default:
         {
            const double *const coefficients =
               windowTypesInfo[settings.mWindowTypes].outCoefficients;
            const double c0 = coefficients[0];
            const double c1 = coefficients[1];
            const double c2 = coefficients[2];
            mOutWindow.resize(mWindowSize);
            for (int ii = 0; ii < mWindowSize; ++ii)
               mOutWindow[ii] = multiplier *
               (c0 + c1 * cos((2.0*M_PI*ii) / mWindowSize)
               + c2 * cos((4.0*M_PI*ii) / mWindowSize));
         }
         break;
      }
   }
}

void EffectNoiseReduction::Worker::StartNewTrack()
{
   float *pFill;
   for(int ii = 0; ii < mHistoryLen; ++ii) {
      Record &record = *mQueue[ii];

      pFill = &record.mSpectrums[0];
      std::fill(pFill, pFill + mSpectrumSize, 0.0f);

      pFill = &record.mRealFFTs[0];
      std::fill(pFill, pFill + mSpectrumSize - 1, 0.0f);

      pFill = &record.mImagFFTs[0];
      std::fill(pFill, pFill + mSpectrumSize - 1, 0.0f);

      pFill = &record.mGains[0];
      std::fill(pFill, pFill + mSpectrumSize, mNoiseAttenFactor);
   }

   pFill = &mOutOverlapBuffer[0];
   std::fill(pFill, pFill + mWindowSize, 0.0f);

   pFill = &mInWaveBuffer[0];
   std::fill(pFill, pFill + mWindowSize, 0.0f);

   // So that the queue gets primed with some windows,
   // zero-padded in front, the first having mStepSize
   // samples of wave data:
   mInWavePos = mWindowSize - mStepSize;

   mInSampleCount = 0;
   
   // This starts negative, to count up until the queue fills:
   mOutStepCount = - (mHistoryLen - 1)
                   // ... and then must pass over the padded windows,
                   // before the first full window:
                   - (mStepsPerWindow - 1);
}

void EffectNoiseReduction::Worker::ProcessSamples
(Statistics &statistics, WaveTrack *outputTrack,
 sampleCount len, float *buffer)
{
   while (len && mOutStepCount * mStepSize < mInSampleCount) {
      int avail = std::min(int(len), mWindowSize - mInWavePos);
      memmove(&mInWaveBuffer[mInWavePos], buffer, avail * sizeof(float));
      buffer += avail;
      len -= avail;
      mInWavePos += avail;

      if (mInWavePos == mWindowSize) {
         FillFirstHistoryWindow();
         if (mDoProfile)
            GatherStatistics(statistics);
         else
            ReduceNoise(statistics, outputTrack);
         ++mOutStepCount;
         RotateHistoryWindows();

         // Rotate for overlap-add
         memmove(&mInWaveBuffer[0], &mInWaveBuffer[mStepSize],
            (mWindowSize - mStepSize) * sizeof(float));
         mInWavePos -= mStepSize;
      }
   }
}

void EffectNoiseReduction::Worker::FillFirstHistoryWindow()
{
   // Transform samples to frequency domain, windowed as needed
   if (mInWindow.size() > 0)
      for (int ii = 0; ii < mWindowSize; ++ii)
         mFFTBuffer[ii] = mInWaveBuffer[ii] * mInWindow[ii];
   else
      memmove(&mFFTBuffer[0], &mInWaveBuffer[0], mWindowSize * sizeof(float));
   RealFFTf(&mFFTBuffer[0], hFFT);

   Record &record = *mQueue[0];

   // Store real and imaginary parts for later inverse FFT, and compute
   // power
   {
      float *pReal = &record.mRealFFTs[1];
      float *pImag = &record.mImagFFTs[1];
      float *pPower = &record.mSpectrums[1];
      int *pBitReversed = &hFFT->BitReversed[1];
      const int last = mSpectrumSize - 1;
      for (int ii = 1; ii < last; ++ii) {
         const int kk = *pBitReversed++;
         const float realPart = *pReal++ = mFFTBuffer[kk];
         const float imagPart = *pImag++ = mFFTBuffer[kk + 1];
         *pPower++ = realPart * realPart + imagPart * imagPart;
      }
      // DC and Fs/2 bins need to be handled specially
      const float dc = mFFTBuffer[0];
      record.mRealFFTs[0] = dc;
      record.mSpectrums[0] = dc*dc;

      const float nyquist = mFFTBuffer[1];
      record.mImagFFTs[0] = nyquist; // For Fs/2, not really imaginary
      record.mSpectrums[last] = nyquist * nyquist;
   }

   if (mNoiseReductionChoice != NRC_ISOLATE_NOISE)
   {
      // Default all gains to the reduction factor,
      // until we decide to raise some of them later
      float *pGain = &record.mGains[0];
      std::fill(pGain, pGain + mSpectrumSize, mNoiseAttenFactor);
   }
}

void EffectNoiseReduction::Worker::RotateHistoryWindows()
{
   Record *save = mQueue[mHistoryLen - 1];
   mQueue.insert(mQueue.begin(), save);
   mQueue.pop_back();
}

void EffectNoiseReduction::Worker::FinishTrackStatistics(Statistics &statistics)
{
   const int windows = statistics.mTrackWindows;
   const int multiplier = statistics.mTotalWindows;
   const int denom = windows + multiplier;

   // Combine averages in case of multiple profile tracks.
   if (windows)
      for (int ii = 0, nn = statistics.mMeans.size(); ii < nn; ++ii) {
         float &mean = statistics.mMeans[ii];
         float &sum = statistics.mSums[ii];
         mean = (mean * multiplier + sum) / denom;
         // Reset for next track
         sum = 0;
      }

   // Reset for next track
   statistics.mTrackWindows = 0;
   statistics.mTotalWindows = denom;
}

void EffectNoiseReduction::Worker::FinishTrack
(Statistics &statistics, WaveTrack *outputTrack)
{
   // Keep flushing empty input buffers through the history
   // windows until we've output exactly as many samples as
   // were input.
   // Well, not exactly, but not more than one step-size of extra samples
   // at the end.
   // We'll delete them later in ProcessOne.

   FloatVector empty(mStepSize);

   while (mOutStepCount * mStepSize < mInSampleCount) {
      ProcessSamples(statistics, outputTrack, mStepSize, &empty[0]);
   }
}

void EffectNoiseReduction::Worker::GatherStatistics(Statistics &statistics)
{
   ++statistics.mTrackWindows;

   {
      // new statistics
      const float *pPower = &mQueue[0]->mSpectrums[0];
      float *pSum = &statistics.mSums[0];
      for (int jj = 0; jj < mSpectrumSize; ++jj) {
         *pSum++ += *pPower++;
      }
   }

#ifdef OLD_METHOD_AVAILABLE
   // The noise threshold for each frequency is the maximum
   // level achieved at that frequency for a minimum of
   // mMinSignalBlocks blocks in a row - the max of a min.

   int finish = mHistoryLen;

   {
      // old statistics
      const float *pPower = &mQueue[0]->mSpectrums[0];
      float *pThreshold = &statistics.mNoiseThreshold[0];
      for (int jj = 0; jj < mSpectrumSize; ++jj) {
         float min = *pPower++;
         for (int ii = 1; ii < finish; ++ii)
            min = std::min(min, mQueue[ii]->mSpectrums[jj]);
         *pThreshold = std::max(*pThreshold, min);
         ++pThreshold;
      }
   }
#endif
}

// Return true iff the given band of the "center" window looks like noise.
// Examine the band in a few neighboring windows to decide.
inline
bool EffectNoiseReduction::Worker::Classify(const Statistics &statistics, int band)
{
   switch (mMethod) {
#ifdef OLD_METHOD_AVAILABLE
   case DM_OLD_METHOD:
      {
         float min = mQueue[0]->mSpectrums[band];
         for (int ii = 1; ii < mNWindowsToExamine; ++ii)
            min = std::min(min, mQueue[ii]->mSpectrums[band]);
         return min <= mSensitivityFactor * statistics.mNoiseThreshold[band];
      }
#endif
   // New methods suppose an exponential distribution of power values
   // in the noise; new sensitivity is meant to be log of probability
   // that noise strays above the threshold.  Call that probability
   // 1 - F.  The quantile function of an exponential distribution is
   // log (1 - F) * mean.  Thus simply multiply mean by sensitivity
   // to get the threshold.
   case DM_MEDIAN:
      // This method examines the window and all windows
      // that partly overlap it, and takes a median, to
      // avoid being fooled by up and down excursions into
      // either the mistake of classifying noise as not noise
      // (leaving a musical noise chime), or the opposite
      // (distorting the signal with a drop out). 
      if (mNWindowsToExamine == 3)
         // No different from second greatest.
         goto secondGreatest;
      else if (mNWindowsToExamine == 5)
      {
         float greatest = 0.0, second = 0.0, third = 0.0;
         for (int ii = 0; ii < mNWindowsToExamine; ++ii) {
            const float power = mQueue[ii]->mSpectrums[band];
            if (power >= greatest)
               third = second, second = greatest, greatest = power;
            else if (power >= second)
               third = second, second = power;
            else if (power >= third)
               third = power;
         }
         return third <= mNewSensitivity * statistics.mMeans[band];
      }
      else {
         wxASSERT(false);
         return true;
      }
   secondGreatest:
   case DM_SECOND_GREATEST:
      {
         // This method just throws out the high outlier.  It
         // should be less prone to distortions and more prone to
         // chimes.
         float greatest = 0.0, second = 0.0;
         for (int ii = 0; ii < mNWindowsToExamine; ++ii) {
            const float power = mQueue[ii]->mSpectrums[band];
            if (power >= greatest)
               second = greatest, greatest = power;
            else if (power >= second)
               second = power;
         }
         return second <= mNewSensitivity * statistics.mMeans[band];
      }
   default:
      wxASSERT(false);
      return true;
   }
}

void EffectNoiseReduction::Worker::ReduceNoise
(const Statistics &statistics, WaveTrack *outputTrack)
{
   // Raise the gain for elements in the center of the sliding history
   // or, if isolating noise, zero out the non-noise
   {
      float *pGain = &mQueue[mCenter]->mGains[0];
      if (mNoiseReductionChoice == NRC_ISOLATE_NOISE) {
         // All above or below the selected frequency range is non-noise
         std::fill(pGain, pGain + mBinLow, 0.0f);
         std::fill(pGain + mBinHigh, pGain + mSpectrumSize, 0.0f);
         pGain += mBinLow;
         for (int jj = mBinLow; jj < mBinHigh; ++jj) {
               const bool isNoise = Classify(statistics, jj);
            *pGain++ = isNoise ? 1.0 : 0.0;
         }
      }
      else {
         // All above or below the selected frequency range is non-noise
         std::fill(pGain, pGain + mBinLow, 1.0f);
         std::fill(pGain + mBinHigh, pGain + mSpectrumSize, 1.0f);
         pGain += mBinLow;
         for (int jj = mBinLow; jj < mBinHigh; ++jj) {
            const bool isNoise = Classify(statistics, jj);
            if (!isNoise) 
               *pGain = 1.0;
            ++pGain;
         }
      }
   }

   if (mNoiseReductionChoice != NRC_ISOLATE_NOISE)
   {
      // In each direction, define an exponential decay of gain from the
      // center; make actual gains the maximum of mNoiseAttenFactor, and
      // the decay curve, and their prior values.

      // First, the attack, which goes backward in time, which is,
      // toward higher indices in the queue.
      for (int jj = 0; jj < mSpectrumSize; ++jj) {
         for (int ii = mCenter + 1; ii < mHistoryLen; ++ii) {
            const float minimum =
               std::max(mNoiseAttenFactor,
                        mQueue[ii - 1]->mGains[jj] * mOneBlockAttack);
            float &gain = mQueue[ii]->mGains[jj];
            if (gain < minimum)
               gain = minimum;
            else
               // We can stop now, our attack curve is intersecting
               // the decay curve of some window previously processed.
               break;
         }
      }

      // Now, release.  We need only look one window ahead.  This part will
      // be visited again when we examine the next window, and
      // carry the decay further.
      {
         float *pNextGain = &mQueue[mCenter - 1]->mGains[0];
         const float *pThisGain = &mQueue[mCenter]->mGains[0];
         for (int nn = mSpectrumSize; nn--;) {
            *pNextGain =
               std::max(*pNextGain,
                        std::max(mNoiseAttenFactor,
                                 *pThisGain++ * mOneBlockRelease));
            ++pNextGain;
         }
      }
   }


   if (mOutStepCount >= -(mStepsPerWindow - 1)) {
      Record &record = *mQueue[mHistoryLen - 1];  // end of the queue
      const int last = mSpectrumSize - 1;

      if (mNoiseReductionChoice != NRC_ISOLATE_NOISE)
         // Apply frequency smoothing to output gain
         // Gains are not less than mNoiseAttenFactor
         ApplyFreqSmoothing(record.mGains);

      // Apply gain to FFT
      {
         const float *pGain = &record.mGains[1];
         const float *pReal = &record.mRealFFTs[1];
         const float *pImag = &record.mImagFFTs[1];
         float *pBuffer = &mFFTBuffer[2];
         int nn = mSpectrumSize - 2;
         if (mNoiseReductionChoice == NRC_LEAVE_RESIDUE) {
            for (; nn--;) {
               // Subtract the gain we would otherwise apply from 1, and
               // negate that to flip the phase.
               const double gain = *pGain++ - 1.0;
               *pBuffer++ = *pReal++ * gain;
               *pBuffer++ = *pImag++ * gain;
            }
            mFFTBuffer[0] = record.mRealFFTs[0] * (record.mGains[0] - 1.0);
            // The Fs/2 component is stored as the imaginary part of the DC component
            mFFTBuffer[1] = record.mImagFFTs[0] * (record.mGains[last] - 1.0);
         }
         else {
            for (; nn--;) {
               const double gain = *pGain++;
               *pBuffer++ = *pReal++ * gain;
               *pBuffer++ = *pImag++ * gain;
            }
            mFFTBuffer[0] = record.mRealFFTs[0] * record.mGains[0];
            // The Fs/2 component is stored as the imaginary part of the DC component
            mFFTBuffer[1] = record.mImagFFTs[0] * record.mGains[last];
         }
      }

      // Invert the FFT into the output buffer
      InverseRealFFTf(&mFFTBuffer[0], hFFT);

      // Overlap-add
      if (mOutWindow.size() > 0) {
         float *pOut = &mOutOverlapBuffer[0];
         float *pWindow = &mOutWindow[0];
         int *pBitReversed = &hFFT->BitReversed[0];
         for (int jj = 0; jj < last; ++jj) {
            int kk = *pBitReversed++;
            *pOut++ += mFFTBuffer[kk] * (*pWindow++);
            *pOut++ += mFFTBuffer[kk + 1] * (*pWindow++);
         }
      }
      else {
         float *pOut = &mOutOverlapBuffer[0];
         int *pBitReversed = &hFFT->BitReversed[0];
         for (int jj = 0; jj < last; ++jj) {
            int kk = *pBitReversed++;
            *pOut++ += mFFTBuffer[kk];
            *pOut++ += mFFTBuffer[kk + 1];
         }
      }

      float *buffer = &mOutOverlapBuffer[0];
      if (mOutStepCount >= 0) {
         // Output the first portion of the overlap buffer, they're done
         outputTrack->Append((samplePtr)buffer, floatSample, mStepSize);
      }

      // Shift the remainder over.
      memmove(buffer, buffer + mStepSize, sizeof(float)*(mWindowSize - mStepSize));
      std::fill(buffer + mWindowSize - mStepSize, buffer + mWindowSize, 0.0f);
   }
}

bool EffectNoiseReduction::Worker::ProcessOne
(EffectNoiseReduction &effect,  Statistics &statistics, TrackFactory &factory,
 int count, WaveTrack * track, sampleCount start, sampleCount len)
{
   if (track == NULL)
      return false;

   StartNewTrack();

   std::auto_ptr<WaveTrack> outputTrack(
      mDoProfile ? NULL
      : factory.NewWaveTrack(track->GetSampleFormat(), track->GetRate()));

   sampleCount bufferSize = track->GetMaxBlockSize();
   FloatVector buffer(bufferSize);

   bool bLoopSuccess = true;
   sampleCount blockSize;
   sampleCount samplePos = start;
   while (bLoopSuccess && samplePos < start + len) {
      //Get a blockSize of samples (smaller than the size of the buffer)
      blockSize = std::min(start + len - samplePos, track->GetBestBlockSize(samplePos));

      //Get the samples from the track and put them in the buffer
      track->Get((samplePtr)&buffer[0], floatSample, samplePos, blockSize);
      samplePos += blockSize;

      mInSampleCount += blockSize;
      ProcessSamples(statistics, outputTrack.get(), blockSize, &buffer[0]);

      // Update the Progress meter, let user cancel
      bLoopSuccess = 
         !effect.TrackProgress(count, (samplePos - start) / (double)len);
   }

   if (bLoopSuccess) {
      if (mDoProfile)
         FinishTrackStatistics(statistics);
      else
         FinishTrack(statistics, &*outputTrack);
   }

   if (bLoopSuccess && !mDoProfile) {
      // Flush the output WaveTrack (since it's buffered)
      outputTrack->Flush();

      // Take the output track and insert it in place of the original
      // sample data (as operated on -- this may not match mT0/mT1)
      double t0 = outputTrack->LongSamplesToTime(start);
      double tLen = outputTrack->LongSamplesToTime(len);
      // Filtering effects always end up with more data than they started with.  Delete this 'tail'.
      outputTrack->HandleClear(tLen, outputTrack->GetEndTime(), false, false);
      bool bResult = track->ClearAndPaste(t0, t0 + tLen, &*outputTrack, true, false);
      wxASSERT(bResult); // TO DO: Actually handle this.
   }

   return bLoopSuccess;
}

//----------------------------------------------------------------------------
// EffectNoiseReduction::Dialog
//----------------------------------------------------------------------------

enum {
   ID_BUTTON_GETPROFILE = 10001,
   ID_RADIOBUTTON_KEEPSIGNAL,
   ID_RADIOBUTTON_KEEPNOISE,
#ifdef RESIDUE_CHOICE
   ID_RADIOBUTTON_RESIDUE,
#endif

#ifdef ADVANCED_SETTINGS
   ID_CHOICE_METHOD,
#endif

   // Slider/text pairs
   ID_GAIN_SLIDER,
   ID_GAIN_TEXT,

   ID_SENSITIVITY_SLIDER,
   ID_SENSITIVITY_TEXT,

   ID_FREQ_SLIDER,
   ID_FREQ_TEXT,

   ID_ATTACK_TIME_SLIDER,
   ID_ATTACK_TIME_TEXT,

   ID_RELEASE_TIME_SLIDER,
   ID_RELEASE_TIME_TEXT,

   END_OF_BASIC_SLIDERS,

#ifdef ADVANCED_SETTINGS
   ID_NEW_SENSITIVITY_SLIDER = END_OF_BASIC_SLIDERS,
   ID_NEW_SENSITIVITY_TEXT,

   END_OF_ADVANCED_SLIDERS,
   END_OF_SLIDERS = END_OF_ADVANCED_SLIDERS,
#else
   END_OF_SLIDERS = END_OF_BASIC_SLIDERS,
#endif

   FIRST_SLIDER = ID_GAIN_SLIDER,
};

namespace {

const struct ControlInfo {
   typedef double (EffectNoiseReduction::Settings::*MemberPointer);

   double Value(long sliderSetting) const
   {
      return
         valueMin +
         (double(sliderSetting) / sliderMax) * (valueMax - valueMin);
   }

   long SliderSetting(double value) const
   {
      return TrapLong(
         0.5 + sliderMax * (value - valueMin) / (valueMax - valueMin),
         0, sliderMax);
   }

   wxString Text(double value) const
   {
      if (formatAsInt)
         return wxString::Format(format, int(value));
      else
         return wxString::Format(format, value);
   }

   void CreateControls(int id, wxTextValidator &vld, ShuttleGui &S) const
   {
      wxTextCtrl *const text =
         S.Id(id + 1).AddTextBox(textBoxCaption, wxT(""), 0);
      S.SetStyle(wxSL_HORIZONTAL);
      text->SetValidator(vld);

      wxSlider *const slider =
         S.Id(id).AddSlider(wxT(""), 0, sliderMax);
      slider->SetName(sliderName);
      slider->SetRange(0, sliderMax);
      slider->SetSizeHints(150, -1);
   }

   MemberPointer field;
   double valueMin;
   double valueMax;
   long sliderMax;
   const wxChar* format;
   bool formatAsInt;
   wxString textBoxCaption;
   wxString sliderName;
} controlInfo[] = {
   { &EffectNoiseReduction::Settings::mNoiseGain,
     0.0, 48.0, 48, wxT("%d"), true,
     _("Noise re&duction (dB):"), _("Noise reduction") },
   { &EffectNoiseReduction::Settings::mSensitivity,
     -20.0, 20.0, 4000, wxT("%.2f"), false,
     _("&Sensitivity (dB):"), _("Sensitivity") },
   { &EffectNoiseReduction::Settings::mFreqSmoothingHz,
     0, 1000, 100, wxT("%d"), true,
     _("Fr&equency smoothing (Hz):"), _("Frequency smoothing") },
   { &EffectNoiseReduction::Settings::mAttackTime,
     0, 1.0, 100, wxT("%.2f"), false,
     _("Attac&k time (secs):"), _("Attack time") },
   { &EffectNoiseReduction::Settings::mReleaseTime,
     0, 1.0, 100, wxT("%.2f"), false,
     _("&Release time (secs):"), _("Release time") },
#ifdef ADVANCED_SETTINGS
   { &EffectNoiseReduction::Settings::mNewSensitivity,
   1.0, 24.0, 92, wxT("%.2f"), false,
   _("New method sensiti&vity:\n(units? you want units?)"), _("New sensitivity") },
   // add here
#endif
};

} // namespace


BEGIN_EVENT_TABLE(EffectNoiseReduction::Dialog, wxDialog)
   EVT_BUTTON(wxID_OK, EffectNoiseReduction::Dialog::OnReduceNoise)
   EVT_BUTTON(wxID_CANCEL, EffectNoiseReduction::Dialog::OnCancel)
   EVT_BUTTON(ID_EFFECT_PREVIEW, EffectNoiseReduction::Dialog::OnPreview)
   EVT_BUTTON(ID_BUTTON_GETPROFILE, EffectNoiseReduction::Dialog::OnGetProfile)

   EVT_RADIOBUTTON(ID_RADIOBUTTON_KEEPNOISE, EffectNoiseReduction::Dialog::OnNoiseReductionChoice)
   EVT_RADIOBUTTON(ID_RADIOBUTTON_KEEPSIGNAL, EffectNoiseReduction::Dialog::OnNoiseReductionChoice)
#ifdef RESIDUE_CHOICE
   EVT_RADIOBUTTON(ID_RADIOBUTTON_RESIDUE, EffectNoiseReduction::Dialog::OnNoiseReductionChoice)
#endif

#ifdef ADVANCED_SETTINGS
   EVT_CHOICE(ID_CHOICE_METHOD, EffectNoiseReduction::Dialog::OnMethodChoice)
#endif

   EVT_SLIDER(ID_GAIN_SLIDER, EffectNoiseReduction::Dialog::OnSlider)
   EVT_TEXT(ID_GAIN_TEXT, EffectNoiseReduction::Dialog::OnText)

   EVT_SLIDER(ID_SENSITIVITY_SLIDER, EffectNoiseReduction::Dialog::OnSlider)
   EVT_TEXT(ID_SENSITIVITY_TEXT, EffectNoiseReduction::Dialog::OnText)

   EVT_SLIDER(ID_FREQ_SLIDER, EffectNoiseReduction::Dialog::OnSlider)
   EVT_TEXT(ID_FREQ_TEXT, EffectNoiseReduction::Dialog::OnText)

   EVT_SLIDER(ID_ATTACK_TIME_SLIDER, EffectNoiseReduction::Dialog::OnSlider)
   EVT_TEXT(ID_ATTACK_TIME_TEXT, EffectNoiseReduction::Dialog::OnText)

   EVT_SLIDER(ID_RELEASE_TIME_SLIDER, EffectNoiseReduction::Dialog::OnSlider)
   EVT_TEXT(ID_RELEASE_TIME_TEXT, EffectNoiseReduction::Dialog::OnText)

#ifdef ADVANCED_SETTINGS
   EVT_SLIDER(ID_NEW_SENSITIVITY_SLIDER, EffectNoiseReduction::Dialog::OnSlider)
   EVT_TEXT(ID_NEW_SENSITIVITY_TEXT, EffectNoiseReduction::Dialog::OnText)
#endif
END_EVENT_TABLE()

EffectNoiseReduction::Dialog::Dialog
(EffectNoiseReduction *effect,
 EffectNoiseReduction::Settings *settings,
 wxWindow *parent, bool bHasProfile, bool bAllowTwiddleSettings)
   : EffectDialog( parent, _("Noise Reduction"), PROCESS_EFFECT)
   , m_pEffect(effect)
   , m_pSettings(settings) // point to
   , mTempSettings(*settings)  // copy
   , mbHasProfile(bHasProfile)
   , mbAllowTwiddleSettings(bAllowTwiddleSettings)
   // NULL out the control members until the controls are created.
   , mKeepSignal(NULL)
   , mKeepNoise(NULL)
#ifdef RESIDUE_CHOICE
   , mResidue(NULL)
#endif
{
   EffectDialog::Init();

   wxButton *const pButtonPreview =
      (wxButton *)wxWindow::FindWindowById(ID_EFFECT_PREVIEW, this);
   wxButton *const pButtonReduceNoise =
      (wxButton *)wxWindow::FindWindowById(wxID_OK, this);

   if (mbHasProfile || mbAllowTwiddleSettings) {
      pButtonPreview->Enable(!mbAllowTwiddleSettings);
      pButtonReduceNoise->SetFocus();
   }
   else {
      pButtonPreview->Enable(false);
      pButtonReduceNoise->Enable(false);
   }
}

void EffectNoiseReduction::Dialog::DisableControlsIfIsolating()
{
   // If Isolate is chosen, disable controls that define
   // "what to do with noise" rather than "what is noise."
   // Else, enable them.
   // This does NOT include sensitivity, new or old, nor
   // the choice of window functions, size, or step.
   // The method choice is not included, because it affects
   // which sensitivity slider is operative, and that is part
   // of what defines noise.

   static const int toDisable[] = {
      ID_GAIN_SLIDER,
      ID_GAIN_TEXT,

      ID_FREQ_SLIDER,
      ID_FREQ_TEXT,

      ID_ATTACK_TIME_SLIDER,
      ID_ATTACK_TIME_TEXT,

      ID_RELEASE_TIME_SLIDER,
      ID_RELEASE_TIME_TEXT,
   };
   static const int nToDisable = sizeof(toDisable) / sizeof(toDisable[0]);
   
   bool bIsolating = mKeepNoise->GetValue();
   for (int ii = nToDisable; ii--;)
      wxWindow::FindWindowById(toDisable[ii], this)->Enable(!bIsolating);
}

#ifdef ADVANCED_SETTINGS
void EffectNoiseReduction::Dialog::EnableSensitivityControls()
{
   wxChoice *const pChoice =
      static_cast<wxChoice*>(wxWindow::FindWindowById(ID_CHOICE_METHOD, this));
   const bool bOldMethod =
      pChoice->GetSelection() == DM_OLD_METHOD;
   wxWindow::FindWindowById(ID_SENSITIVITY_SLIDER, this)->Enable(bOldMethod);
   wxWindow::FindWindowById(ID_SENSITIVITY_TEXT, this)->Enable(bOldMethod);
   wxWindow::FindWindowById(ID_NEW_SENSITIVITY_SLIDER, this)->Enable(!bOldMethod);
   wxWindow::FindWindowById(ID_NEW_SENSITIVITY_TEXT, this)->Enable(!bOldMethod);
}
#endif

void EffectNoiseReduction::Dialog::OnGetProfile(wxCommandEvent & WXUNUSED(event))
{
   if (!TransferDataFromWindow())
      return;

   // Return code distinguishes this first step from the actual effect
   EndModal(1);
}

// This handles the whole radio group
void EffectNoiseReduction::Dialog::OnNoiseReductionChoice( wxCommandEvent & WXUNUSED(event))
{
   if (mKeepSignal->GetValue())
      mTempSettings.mNoiseReductionChoice = NRC_REDUCE_NOISE;
   else if (mKeepNoise->GetValue())
      mTempSettings.mNoiseReductionChoice = NRC_ISOLATE_NOISE;
#ifdef RESIDUE_CHOICE
   else
      mTempSettings.mNoiseReductionChoice = NRC_LEAVE_RESIDUE;
#endif
   DisableControlsIfIsolating();
}

#ifdef ADVANCED_SETTINGS
void EffectNoiseReduction::Dialog::OnMethodChoice(wxCommandEvent &)
{
   EnableSensitivityControls();
}
#endif

void EffectNoiseReduction::Dialog::OnPreview(wxCommandEvent & WXUNUSED(event))
{
   if (!TransferDataFromWindow())
      return;

   // Save & restore parameters around Preview, because we didn't do OK.
   EffectNoiseReduction::Settings oldSettings(*m_pSettings);

   *m_pSettings = mTempSettings;
   m_pSettings->mDoProfile = false;

   m_pEffect->Preview();

   *m_pSettings = oldSettings;
}

void EffectNoiseReduction::Dialog::OnReduceNoise( wxCommandEvent & WXUNUSED(event))
{
   if (!TransferDataFromWindow())
      return;

   EndModal(2);
}

void EffectNoiseReduction::Dialog::OnCancel(wxCommandEvent & WXUNUSED(event))
{
   EndModal(0);
}

void EffectNoiseReduction::Dialog::PopulateOrExchange(ShuttleGui & S)
{
   S.StartStatic(_("Step 1"));
   {
      S.AddVariableText(_(
         "Select a few seconds of just noise so Audacity knows what to filter out,\nthen click Get Noise Profile:"));
      //m_pButton_GetProfile =
      S.Id(ID_BUTTON_GETPROFILE).AddButton(_("&Get Noise Profile"));
   }
   S.EndStatic();

   S.StartStatic(_("Step 2"));
   {
      S.AddVariableText(_(
         "Select all of the audio you want filtered, choose how much noise you want\nfiltered out, and then click 'OK' to reduce noise.\n"));

      S.StartMultiColumn(3, wxEXPAND);
      S.SetStretchyCol(2);
      {
         wxTextValidator vld(wxFILTER_NUMERIC);
         for (int id = FIRST_SLIDER; id < END_OF_BASIC_SLIDERS; id += 2) {
            const ControlInfo &info = controlInfo[(id - FIRST_SLIDER) / 2];
            info.CreateControls(id, vld, S);
         }
      }
      S.EndMultiColumn();

      S.StartMultiColumn(
#ifdef RESIDUE_CHOICE
         4,
#else
         3,
#endif
         wxALIGN_CENTER_HORIZONTAL);
      {
         S.AddPrompt(_("Noise:"));
         mKeepSignal = S.Id(ID_RADIOBUTTON_KEEPSIGNAL)
               .AddRadioButton(_("Re&duce"));
         mKeepNoise = S.Id(ID_RADIOBUTTON_KEEPNOISE)
               .AddRadioButtonToGroup(_("&Isolate"));
#ifdef RESIDUE_CHOICE
         mResidue = S.Id(ID_RADIOBUTTON_RESIDUE)
               .AddRadioButtonToGroup(_("Resid&ue"));
#endif
      }
      S.EndMultiColumn();
   }
   S.EndStatic();


#ifdef ADVANCED_SETTINGS
   S.StartStatic(_("Advanced Settings"));
   {
      S.StartMultiColumn(2);
      {
         {
            wxArrayString windowTypeChoices;
            for (int ii = 0; ii < WT_N_WINDOW_TYPES; ++ii)
               windowTypeChoices.Add(windowTypesInfo[ii].name);
            S.TieChoice(_("&Window types") + wxString(wxT(":")),
               mTempSettings.mWindowTypes,
               &windowTypeChoices);
         }

         {
            wxArrayString windowSizeChoices;
            windowSizeChoices.Add(_("8"));
            windowSizeChoices.Add(_("16"));
            windowSizeChoices.Add(_("32"));
            windowSizeChoices.Add(_("64"));
            windowSizeChoices.Add(_("128"));
            windowSizeChoices.Add(_("256"));
            windowSizeChoices.Add(_("512"));
            windowSizeChoices.Add(_("1024"));
            windowSizeChoices.Add(_("2048 (default)"));
            windowSizeChoices.Add(_("4096"));
            windowSizeChoices.Add(_("8192"));
            windowSizeChoices.Add(_("16384"));
            S.TieChoice(_("&Window size") + wxString(wxT(":")),
               mTempSettings.mWindowSizeChoice,
               &windowSizeChoices);
         }

         {
            wxArrayString stepsPerWindowChoices;
            stepsPerWindowChoices.Add(_("2"));
            stepsPerWindowChoices.Add(_("4 (default)"));
            stepsPerWindowChoices.Add(_("8"));
            stepsPerWindowChoices.Add(_("16"));
            stepsPerWindowChoices.Add(_("32"));
            stepsPerWindowChoices.Add(_("64"));
            S.TieChoice(_("Steps &per window") + wxString(wxT(":")),
               mTempSettings.mStepsPerWindowChoice,
               &stepsPerWindowChoices);
         }

         S.Id(ID_CHOICE_METHOD);
         {
            wxArrayString methodChoices;
            int nn = DM_N_METHODS;
#ifndef OLD_METHOD_AVAILABLE
            --nn;
#endif
            for (int ii = 0; ii < nn; ++ii)
               methodChoices.Add(discriminationMethodInfo[ii].name);
            S.TieChoice(_("&Discrimination method") + wxString(wxT(":")),
               mTempSettings.mMethod,
               &methodChoices);
         }
      }
      S.EndMultiColumn();

      S.StartMultiColumn(3, wxEXPAND);
      S.SetStretchyCol(2);
      {
         wxTextValidator vld(wxFILTER_NUMERIC);
         for (int id = END_OF_BASIC_SLIDERS; id < END_OF_ADVANCED_SLIDERS; id += 2) {
            const ControlInfo &info = controlInfo[(id - FIRST_SLIDER) / 2];
            info.CreateControls(id, vld, S);
         }
      }
      S.EndMultiColumn();
   }
   S.EndStatic();
#endif
}

bool EffectNoiseReduction::Dialog::TransferDataToWindow()
{
   // Do the choice controls:
   if (!EffectDialog::TransferDataToWindow())
      return false;

   for (int id = FIRST_SLIDER; id < END_OF_SLIDERS; id += 2) {
      wxSlider* slider =
         static_cast<wxSlider*>(wxWindow::FindWindowById(id, this));
      wxTextCtrl* text =
         static_cast<wxTextCtrl*>(wxWindow::FindWindowById(id + 1, this));
      const ControlInfo &info = controlInfo[(id - FIRST_SLIDER) / 2];
      const double field = mTempSettings.*(info.field);
      text->SetValue(info.Text(field));
      slider->SetValue(info.SliderSetting(field));
   }

   mKeepSignal->SetValue(mTempSettings.mNoiseReductionChoice == NRC_REDUCE_NOISE);
   mKeepNoise->SetValue(mTempSettings.mNoiseReductionChoice == NRC_ISOLATE_NOISE);
#ifdef RESIDUE_CHOICE
   mResidue->SetValue(mTempSettings.mNoiseReductionChoice == NRC_LEAVE_RESIDUE);
#endif

   // Set the enabled states of controls
   DisableControlsIfIsolating();
#ifdef ADVANCED_SETTINGS
   EnableSensitivityControls();
#endif

   return true;
}

bool EffectNoiseReduction::Dialog::TransferDataFromWindow()
{
   // Do the choice controls:
   if (!EffectDialog::TransferDataFromWindow())
      return false;

   wxCommandEvent dummy;
   OnNoiseReductionChoice(dummy);

   return mTempSettings.Validate();
}

void EffectNoiseReduction::Dialog::OnText(wxCommandEvent &event)
{
   int id = event.GetId();
   int idx = (id - FIRST_SLIDER - 1) / 2;
   const ControlInfo &info = controlInfo[idx];
   wxTextCtrl* text =
      static_cast<wxTextCtrl*>(wxWindow::FindWindowById(id, this));
   wxSlider* slider =
      static_cast<wxSlider*>(wxWindow::FindWindowById(id - 1, this));
   double &field = mTempSettings.*(info.field);

   text->GetValue().ToDouble(&field);
   slider->SetValue(info.SliderSetting(field));
}

void EffectNoiseReduction::Dialog::OnSlider(wxCommandEvent &event)
{
   int id = event.GetId();
   int idx = (id - FIRST_SLIDER) / 2;
   const ControlInfo &info = controlInfo[idx];
   wxSlider* slider =
      static_cast<wxSlider*>(wxWindow::FindWindowById(id, this));
   wxTextCtrl* text =
      static_cast<wxTextCtrl*>(wxWindow::FindWindowById(id + 1, this));
   double &field = mTempSettings.*(info.field);

   field = info.Value(slider->GetValue());
   text->SetValue(info.Text(field));
}

