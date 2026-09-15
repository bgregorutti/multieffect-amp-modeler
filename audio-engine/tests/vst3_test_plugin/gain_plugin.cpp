// Trivial in-repo VST3 plugin: a single adjustable "Gain" parameter
// (plain range [0, 2]x, default 1x/unity), mono in/out. Exists only so
// test_vst3_host.cpp can exercise Vst3PluginHost end to end against a real
// `.vst3` bundle without depending on any third-party plugin binary (see
// CMakeLists.txt "VST3 plugin hosting").
//
// Hand-written directly against the vendored pluginterfaces -- no
// public.sdk (no CPluginFactory, no AudioEffect/EditController base
// classes) -- since only pluginterfaces is vendored. GainProcessor
// implementing IComponent + IAudioProcessor + IEditController via multiple
// inheritance (without virtual inheritance) is the standard VST3 idiom for
// a "single component" plugin -- public.sdk's own AudioEffect combines
// IComponent + IAudioProcessor the same way -- so queryInterface/addRef/
// release are written by hand below instead of via the single-interface
// DECLARE_/IMPLEMENT_FUNKNOWN_METHODS macros (those only branch on one
// interface id, which isn't enough for a class implementing three).
#include <atomic>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

constexpr ParamID kGainParamId = 0;
constexpr TUID kGainComponentCid = INLINE_UID(0x4D756C74, 0x69656666, 0x65637447, 0x61696E31);

void setString128(TChar* dest, const char* src) {
    std::size_t i = 0;
    for (; src[i] != '\0' && i < 127; ++i) dest[i] = static_cast<TChar>(src[i]);
    dest[i] = 0;
}

double normalizedToPlainGain(ParamValue normalized) { return normalized * 2.0; }  // plain range [0, 2]x

class GainProcessor : public IComponent, public IAudioProcessor, public IEditController {
public:
    GainProcessor() = default;
    virtual ~GainProcessor() = default;

    // --- FUnknown (one shared implementation for all three interfaces) ---
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, FUnknown_iid) || FUnknownPrivate::iidEqual(_iid, IPluginBase_iid) ||
            FUnknownPrivate::iidEqual(_iid, IComponent_iid)) {
            addRef();
            *obj = static_cast<IComponent*>(this);
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IAudioProcessor_iid)) {
            addRef();
            *obj = static_cast<IAudioProcessor*>(this);
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IEditController_iid)) {
            addRef();
            *obj = static_cast<IEditController*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return static_cast<uint32>(++refCount_); }
    uint32 PLUGIN_API release() override {
        int32 result = --refCount_;
        if (result == 0) {
            delete this;
            return 0;
        }
        return static_cast<uint32>(result);
    }

    // --- IPluginBase (shared body: IComponent's and IEditController's
    // initialize/terminate have identical signatures) ---
    tresult PLUGIN_API initialize(FUnknown* /*context*/) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    // --- IComponent ---
    tresult PLUGIN_API getControllerClassId(TUID /*classId*/) override {
        return kResultFalse;  // single-component plugin: no separate controller class
    }
    tresult PLUGIN_API setIoMode(IoMode /*mode*/) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection /*dir*/) override { return type == kAudio ? 1 : 0; }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (type != kAudio || index != 0) return kInvalidArgument;
        bus.mediaType = kAudio;
        bus.direction = dir;
        bus.channelCount = 1;
        setString128(bus.name, dir == kInput ? "Input" : "Output");
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool state) override {
        active_ = (state != 0);
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream*) override { return kResultOk; }

    // --- IAudioProcessor ---
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs,
                                           int32 numOuts) override {
        if (numIns != 1 || numOuts != 1) return kResultFalse;
        inArrangement_ = inputs[0];
        outArrangement_ = outputs[0];
        return kResultTrue;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) override {
        if (index != 0) return kInvalidArgument;
        arr = (dir == kInput) ? inArrangement_ : outArrangement_;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        sampleRate_ = setup.sampleRate;
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool state) override {
        processing_ = (state != 0);
        return kResultOk;
    }
    tresult PLUGIN_API process(ProcessData& data) override {
        if (data.inputParameterChanges) {
            int32 count = data.inputParameterChanges->getParameterCount();
            for (int32 i = 0; i < count; ++i) {
                IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
                if (!queue || queue->getParameterId() != kGainParamId) continue;
                int32 pointCount = queue->getPointCount();
                if (pointCount <= 0) continue;
                int32 offset = 0;
                ParamValue value = 0.0;
                // Only the final point matters here: this host never sends
                // more than one point per parameter per block (see
                // Vst3PluginHost::setParam) -- no interpolation to do.
                if (queue->getPoint(pointCount - 1, offset, value) == kResultOk) gainNormalized_ = value;
            }
        }
        if (data.numSamples > 0 && data.inputs && data.outputs && data.numInputs > 0 && data.numOutputs > 0) {
            auto gain = static_cast<Sample32>(normalizedToPlainGain(gainNormalized_));
            for (int32 ch = 0; ch < data.inputs[0].numChannels; ++ch) {
                Sample32* in = data.inputs[0].channelBuffers32[ch];
                Sample32* out = data.outputs[0].channelBuffers32[ch];
                for (int32 i = 0; i < data.numSamples; ++i) out[i] = in[i] * gain;
            }
        }
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return 0; }

    // --- IEditController ---
    tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }
    // setState/getState above (shared IComponent/IEditController signature) cover this interface's slot too.
    int32 PLUGIN_API getParameterCount() override { return 1; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
        if (index != 0) return kInvalidArgument;
        info.id = kGainParamId;
        setString128(info.title, "Gain");
        setString128(info.shortTitle, "Gain");
        setString128(info.units, "x");
        info.stepCount = 0;
        info.defaultNormalizedValue = 0.5;  // -> plain 1.0x (unity)
        info.unitId = 0;
        info.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue, String128) override { return kNotImplemented; }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kNotImplemented; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue valueNormalized) override {
        return normalizedToPlainGain(valueNormalized);
    }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue plainValue) override { return plainValue / 2.0; }
    ParamValue PLUGIN_API getParamNormalized(ParamID) override { return gainNormalized_; }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override {
        if (id != kGainParamId) return kInvalidArgument;
        gainNormalized_ = value;
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

private:
    std::atomic<int32> refCount_{1};
    bool active_ = false;
    bool processing_ = false;
    double sampleRate_ = 48000.0;
    SpeakerArrangement inArrangement_ = SpeakerArr::kMono;
    SpeakerArrangement outArrangement_ = SpeakerArr::kMono;
    ParamValue gainNormalized_ = 0.5;  // default: plain 1.0x (unity)
};

// Hand-written IPluginFactory (public.sdk's CPluginFactory isn't
// available -- only pluginterfaces is vendored) exporting exactly one
// class: GainProcessor, category "Audio Module Class".
class GainPluginFactory : public IPluginFactory {
public:
    GainPluginFactory() = default;
    virtual ~GainPluginFactory() = default;

    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, FUnknown_iid) || FUnknownPrivate::iidEqual(_iid, IPluginFactory_iid)) {
            addRef();
            *obj = static_cast<IPluginFactory*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return static_cast<uint32>(++refCount_); }
    uint32 PLUGIN_API release() override {
        int32 result = --refCount_;
        if (result == 0) {
            delete this;
            return 0;
        }
        return static_cast<uint32>(result);
    }

    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        if (!info) return kInvalidArgument;
        *info = PFactoryInfo("multieffect-amp-modeler tests", "", "", PFactoryInfo::kNoFlags);
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 1; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (index != 0 || !info) return kInvalidArgument;
        *info = PClassInfo(kGainComponentCid, PClassInfo::kManyInstances, kVstAudioEffectClass, "Test Gain");
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString _iid, void** obj) override {
        if (!FUnknownPrivate::iidEqual(cid, kGainComponentCid)) {
            *obj = nullptr;
            return kInvalidArgument;
        }
        auto* processor = new GainProcessor();
        tresult result = processor->queryInterface(_iid, obj);
        processor->release();  // queryInterface above already added the ref `obj` now holds
        return result;
    }

private:
    std::atomic<int32> refCount_{1};
};

IPluginFactory* gFactory = nullptr;

}  // namespace

extern "C" SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() {
    if (!gFactory) {
        gFactory = new GainPluginFactory();
    } else {
        gFactory->addRef();
    }
    return gFactory;
}
