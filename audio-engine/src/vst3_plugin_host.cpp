// Real VST3 hosting: loads a `.vst3` bundle's shared module via dlopen,
// walks its IPluginFactory for the first "Audio Module Class", instantiates
// its IComponent/IAudioProcessor (and IEditController, if any), and drives
// them directly through the plain COM-style interfaces vendored from
// steinbergmedia/vst3_pluginterfaces -- no JUCE, no public.sdk (see
// vst3_host.hpp and CMakeLists.txt "VST3 plugin hosting" for why).
//
// Only built when AUDIO_ENGINE_WITH_VST3 is ON; vst3_plugin_host_stub.cpp
// is built instead otherwise.
#include "audio_engine/vst3_host.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace audio_engine {

namespace {

// A real-time audio block in this engine is always small (see
// ARCHITECTURE.md) -- this is a generous ceiling for IAudioProcessor's
// mandatory upfront maxSamplesPerBlock, not a tight assumption. process()
// below chunks any larger call into pieces of at most this size instead of
// silently truncating or violating the plugin's declared contract.
constexpr int32 kMaxBlockSize = 4096;

std::string utf16ToUtf8(const TChar* str) {
    std::string out;
    for (int i = 0; i < 128 && str[i] != 0; ++i) {
        auto c = static_cast<unsigned>(static_cast<uint16_t>(str[i]));
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Finds `<bundlePath>/Contents/<arch>-linux/*.so` -- the standard VST3
// bundle layout on Linux (see steinbergmedia/vst3sdk's own module loader) --
// or, failing that, `<bundlePath>/Contents/MacOS/*` -- the standard macOS
// bundle layout (an Info.plist-described app bundle, same shape VST3 uses
// there: one Mach-O binary directly under Contents/MacOS, no arch
// subdirectory or file extension). Checking Linux first keeps this engine's
// own in-repo test plugin (deliberately packaged `-linux`-style even when
// built on macOS, see CMakeLists.txt "VST3 plugin hosting") resolving the
// same way regardless of host OS; real third-party plugins installed on a
// Mac fall through to the MacOS branch.
std::string resolveModuleSharedLibrary(const std::string& bundlePath) {
    namespace fs = std::filesystem;
    fs::path contents = fs::path(bundlePath) / "Contents";
    if (!fs::is_directory(contents)) {
        throw std::runtime_error("VST3 bundle '" + bundlePath + "' has no Contents/ directory");
    }
    std::error_code ec;
    for (const auto& archDir : fs::directory_iterator(contents, ec)) {
        if (!archDir.is_directory()) continue;
        if (archDir.path().filename().string().find("-linux") == std::string::npos) continue;
        std::error_code innerEc;
        for (const auto& file : fs::directory_iterator(archDir.path(), innerEc)) {
            if (file.path().extension() == ".so") return file.path().string();
        }
    }
    fs::path macosDir = contents / "MacOS";
    std::error_code macosEc;
    if (fs::is_directory(macosDir, macosEc)) {
        std::error_code innerEc;
        for (const auto& file : fs::directory_iterator(macosDir, innerEc)) {
            if (file.is_regular_file()) return file.path().string();
        }
    }
    throw std::runtime_error(
        "VST3 bundle '" + bundlePath + "' has no <arch>-linux/*.so module or Contents/MacOS/* binary");
}

// Minimal host-side FUnknown implementations. None of these need a real
// public.sdk -- the DECLARE_/IMPLEMENT_FUNKNOWN_METHODS macros (from
// pluginterfaces/base/funknown.h) are all plain FUnknown boilerplate.

class HostApplication : public IHostApplication {
public:
    HostApplication() { FUNKNOWN_CTOR }
    ~HostApplication() { FUNKNOWN_DTOR }
    DECLARE_FUNKNOWN_METHODS

    tresult PLUGIN_API getName(String128 name) override {
        static const char kName[] = "multieffect-amp-modeler";
        size_t i = 0;
        for (; i < sizeof(kName) - 1 && i < 127; ++i) name[i] = static_cast<TChar>(kName[i]);
        name[i] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID /*cid*/, TUID /*iid*/, void** /*obj*/) override {
        return kNotImplemented;
    }
};
// Using the header-declared `IHostApplication_iid` TUID constant (internal
// linkage, always available) rather than the `IHostApplication::iid` FUID
// static member -- the latter is only ever *given storage* by a
// DEF_CLASS_IID line, and none of the vendored .cpp files provide one for
// any Vst-namespace interface (only the base ones in coreiids.cpp do), so
// referencing the FUID member here would link-fail with an undefined
// reference. Same reasoning applies everywhere else below that needs an
// interface id.
IMPLEMENT_FUNKNOWN_METHODS(HostApplication, IHostApplication, IHostApplication_iid)

// No-op: this host drives every parameter change itself (setLiveParam ->
// setParam), so there is no GUI-originated edit for the plugin to report
// back through here. Still implemented (rather than passed as nullptr) so
// a plugin that assumes a non-null handler doesn't crash.
class ComponentHandler : public IComponentHandler {
public:
    ComponentHandler() { FUNKNOWN_CTOR }
    ~ComponentHandler() { FUNKNOWN_DTOR }
    DECLARE_FUNKNOWN_METHODS

    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
};
IMPLEMENT_FUNKNOWN_METHODS(ComponentHandler, IComponentHandler, IComponentHandler_iid)

// One parameter's automation curve for one process() call. This host only
// ever sends a single "jump to this value now" point per live-tweaked
// parameter (see Vst3PluginHost::setParam), not a real interpolated
// automation curve -- ParamValueQueue's multi-point machinery is still
// implemented for real per the interface contract, since a plugin is
// entitled to call getPointCount()/getPoint() the normal way.
class ParamValueQueue : public IParamValueQueue {
public:
    explicit ParamValueQueue(ParamID id) : id_(id) { FUNKNOWN_CTOR }
    ~ParamValueQueue() { FUNKNOWN_DTOR }
    DECLARE_FUNKNOWN_METHODS

    ParamID PLUGIN_API getParameterId() override { return id_; }
    int32 PLUGIN_API getPointCount() override { return static_cast<int32>(points_.size()); }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) override {
        if (index < 0 || static_cast<size_t>(index) >= points_.size()) return kInvalidArgument;
        sampleOffset = points_[static_cast<size_t>(index)].first;
        value = points_[static_cast<size_t>(index)].second;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue value, int32& index) override {
        index = static_cast<int32>(points_.size());
        points_.emplace_back(sampleOffset, value);
        return kResultOk;
    }

private:
    ParamID id_;
    std::vector<std::pair<int32, ParamValue>> points_;
};
IMPLEMENT_FUNKNOWN_METHODS(ParamValueQueue, IParamValueQueue, IParamValueQueue_iid)

// The list of per-parameter queues handed to the plugin as
// ProcessData::inputParameterChanges for one process() call. Cleared and
// refilled by Vst3PluginHost::process() from whatever setParam() calls
// arrived since the previous block -- see the comment there.
class ParameterChanges : public IParameterChanges {
public:
    ParameterChanges() { FUNKNOWN_CTOR }
    ~ParameterChanges() {
        clear();
        FUNKNOWN_DTOR
    }
    DECLARE_FUNKNOWN_METHODS

    int32 PLUGIN_API getParameterCount() override { return static_cast<int32>(queues_.size()); }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        if (index < 0 || static_cast<size_t>(index) >= queues_.size()) return nullptr;
        return queues_[static_cast<size_t>(index)];
    }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override {
        auto* queue = new ParamValueQueue(id);  // refcount starts at 1, owned by queues_ until clear()
        index = static_cast<int32>(queues_.size());
        queues_.push_back(queue);
        return queue;
    }

    void clear() {
        for (auto* queue : queues_) queue->release();
        queues_.clear();
    }

private:
    std::vector<ParamValueQueue*> queues_;
};
IMPLEMENT_FUNKNOWN_METHODS(ParameterChanges, IParameterChanges, IParameterChanges_iid)

// dlclose must run only after every interface pointer obtained from the
// module has been released -- declaring this as the *first* class member
// makes it the *last* one destroyed (C++ destroys members in reverse
// declaration order), after all the IPtr<...> members below have already
// released their references.
struct DlHandle {
    void* handle = nullptr;
    ~DlHandle() {
        if (handle) ::dlclose(handle);
    }
};

enum class ChannelLayout { kMono, kStereo };

class Vst3PluginHost : public IHostedPlugin {
public:
    explicit Vst3PluginHost(const std::string& bundlePath) {
        std::string modulePath = resolveModuleSharedLibrary(bundlePath);
        module_.handle = ::dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!module_.handle) {
            throw std::runtime_error("failed to dlopen VST3 module '" + modulePath + "': " + ::dlerror());
        }

        auto getFactory = reinterpret_cast<GetFactoryProc>(::dlsym(module_.handle, "GetPluginFactory"));
        if (!getFactory) {
            throw std::runtime_error("VST3 module '" + modulePath + "' does not export GetPluginFactory");
        }
        factory_ = owned(getFactory());
        if (!factory_) {
            throw std::runtime_error("VST3 module '" + modulePath + "' returned a null plugin factory");
        }

        hostApplication_ = owned(new HostApplication());
        componentHandler_ = owned(new ComponentHandler());

        TUID componentCid{};
        bool foundComponentClass = false;
        int32 classCount = factory_->countClasses();
        for (int32 i = 0; i < classCount; ++i) {
            PClassInfo info{};
            if (factory_->getClassInfo(i, &info) != kResultOk) continue;
            if (std::string(info.category) == kVstAudioEffectClass) {
                std::memcpy(componentCid, info.cid, sizeof(TUID));
                foundComponentClass = true;
                break;
            }
        }
        if (!foundComponentClass) {
            throw std::runtime_error("VST3 module '" + modulePath + "' exports no Audio Module Class");
        }

        IComponent* rawComponent = nullptr;
        if (factory_->createInstance(componentCid, IComponent_iid, reinterpret_cast<void**>(&rawComponent)) !=
                kResultOk ||
            !rawComponent) {
            throw std::runtime_error("failed to instantiate the VST3 component in '" + modulePath + "'");
        }
        component_ = owned(rawComponent);

        if (component_->initialize(hostApplication_.get()) != kResultOk) {
            throw std::runtime_error("VST3 component in '" + modulePath + "' refused initialize()");
        }
        componentInitialized_ = true;

        IAudioProcessor* rawProcessor = nullptr;
        if (component_->queryInterface(IAudioProcessor_iid, reinterpret_cast<void**>(&rawProcessor)) !=
                kResultOk ||
            !rawProcessor) {
            throw std::runtime_error("VST3 component in '" + modulePath + "' does not implement IAudioProcessor");
        }
        processor_ = owned(rawProcessor);

        // Edit controller: either a separate class (per getControllerClassId)
        // or, for a "single component" plugin, IComponent itself also
        // implements IEditController -- both are valid VST3.
        TUID controllerCid{};
        IEditController* rawController = nullptr;
        if (component_->getControllerClassId(controllerCid) == kResultOk) {
            factory_->createInstance(controllerCid, IEditController_iid, reinterpret_cast<void**>(&rawController));
        }
        if (rawController) {
            controller_ = owned(rawController);
            controllerIsSeparate_ = true;
            if (controller_->initialize(hostApplication_.get()) != kResultOk) {
                throw std::runtime_error("VST3 edit controller in '" + modulePath + "' refused initialize()");
            }
            controllerInitialized_ = true;
        } else {
            IEditController* sameObjectController = nullptr;
            component_->queryInterface(IEditController_iid, reinterpret_cast<void**>(&sameObjectController));
            if (sameObjectController) controller_ = owned(sameObjectController);
        }
        if (controller_) controller_->setComponentHandler(componentHandler_.get());

        layout_ = negotiateChannelLayout(modulePath);
        component_->activateBus(kAudio, kInput, 0, true);
        component_->activateBus(kAudio, kOutput, 0, true);

        cachedParameters_ = buildParameterList();
        for (const auto& param : cachedParameters_) knownParamIds_.insert(static_cast<ParamID>(std::stoul(param.key)));
    }

    ~Vst3PluginHost() override {
        if (processingActive_) processor_->setProcessing(false);
        if (activated_) component_->setActive(false);
        if (controller_ && controllerIsSeparate_ && controllerInitialized_) controller_->terminate();
        if (component_ && componentInitialized_) component_->terminate();
        // Everything else (IPtr members, then module_) unwinds automatically
        // in reverse declaration order below.
    }

    void prepare(double sampleRate) override {
        if (processingActive_) {
            processor_->setProcessing(false);
            processingActive_ = false;
        }
        if (activated_) {
            component_->setActive(false);
            activated_ = false;
        }

        ProcessSetup setup{};
        setup.processMode = kRealtime;
        setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = kMaxBlockSize;
        setup.sampleRate = sampleRate;
        if (processor_->setupProcessing(setup) != kResultOk) {
            throw std::runtime_error("VST3 plugin refused setupProcessing() at " + std::to_string(sampleRate) + "Hz");
        }

        if (component_->setActive(true) != kResultOk) {
            throw std::runtime_error("VST3 plugin refused setActive(true)");
        }
        activated_ = true;
        if (processor_->setProcessing(true) != kResultOk) {
            throw std::runtime_error("VST3 plugin refused setProcessing(true)");
        }
        processingActive_ = true;
    }

    void process(float* buffer, std::size_t numSamples) override {
        std::size_t offset = 0;
        while (offset < numSamples) {
            std::size_t chunk = std::min<std::size_t>(numSamples - offset, static_cast<std::size_t>(kMaxBlockSize));
            processChunk(buffer + offset, chunk);
            offset += chunk;
        }
    }

    void reset() override {
        // No dedicated "flush" call in VST3; deactivating and reactivating
        // is the documented way to make a plugin drop internal state (delay
        // lines, reverb tails). Best-effort: a plugin that rejects a rapid
        // activate cycle just keeps whatever state it had.
        processor_->setProcessing(false);
        component_->setActive(false);
        if (component_->setActive(true) == kResultOk) {
            processor_->setProcessing(true);
        } else {
            activated_ = false;
            processingActive_ = false;
        }
    }

    std::vector<ParameterDescriptor> listParameters() const override { return cachedParameters_; }

    bool setParam(const std::string& key, double value) override {
        ParamID id{};
        try {
            id = static_cast<ParamID>(std::stoul(key));
        } catch (const std::exception&) {
            return false;
        }
        if (!controller_ || knownParamIds_.find(id) == knownParamIds_.end()) return false;

        ParamValue normalized = controller_->plainParamToNormalized(id, value);
        controller_->setParamNormalized(id, normalized);
        pendingChanges_.emplace_back(id, normalized);
        return true;
    }

private:
    ChannelLayout negotiateChannelLayout(const std::string& modulePath) {
        int32 inChannels = busChannelCount(kInput);
        int32 outChannels = busChannelCount(kOutput);
        if (inChannels == 1 && outChannels == 1) {
            SpeakerArrangement mono = SpeakerArr::kMono;
            processor_->setBusArrangements(&mono, 1, &mono, 1);
            return ChannelLayout::kMono;
        }
        if (inChannels == 2 && outChannels == 2) {
            SpeakerArrangement stereo = SpeakerArr::kStereo;
            processor_->setBusArrangements(&stereo, 1, &stereo, 1);
            return ChannelLayout::kStereo;
        }
        throw std::runtime_error("VST3 plugin '" + modulePath + "' has an unsupported bus layout (" +
                                  std::to_string(inChannels) + " in / " + std::to_string(outChannels) +
                                  " out channels) -- only mono and stereo main busses are hosted");
    }

    int32 busChannelCount(BusDirection dir) {
        if (component_->getBusCount(kAudio, dir) < 1) return 0;
        BusInfo info{};
        if (component_->getBusInfo(kAudio, dir, 0, info) != kResultOk) return 0;
        return info.channelCount;
    }

    std::vector<ParameterDescriptor> buildParameterList() const {
        std::vector<ParameterDescriptor> result;
        if (!controller_) return result;
        int32 count = controller_->getParameterCount();
        result.reserve(static_cast<size_t>(std::max<int32>(count, 0)));
        for (int32 i = 0; i < count; ++i) {
            ParameterInfo info{};
            if (controller_->getParameterInfo(i, info) != kResultOk) continue;
            if (info.flags & ParameterInfo::kIsHidden) continue;

            ParameterDescriptor descriptor;
            descriptor.key = std::to_string(info.id);
            descriptor.label = utf16ToUtf8(info.title);
            descriptor.unit = utf16ToUtf8(info.units);
            descriptor.minValue = controller_->normalizedParamToPlain(info.id, 0.0);
            descriptor.maxValue = controller_->normalizedParamToPlain(info.id, 1.0);
            descriptor.defaultValue = controller_->normalizedParamToPlain(info.id, info.defaultNormalizedValue);
            descriptor.stepCount = info.stepCount;
            result.push_back(std::move(descriptor));
        }
        return result;
    }

    // Runs exactly `numSamples` (<= kMaxBlockSize) through the plugin,
    // converting between the engine's mono buffer and the plugin's
    // negotiated bus layout, and delivering any pending setParam() points.
    void processChunk(float* buffer, std::size_t numSamples) {
        parameterChanges_.clear();
        for (const auto& [id, normalized] : pendingChanges_) {
            int32 index = 0;
            IParamValueQueue* queue = parameterChanges_.addParameterData(id, index);
            int32 pointIndex = 0;
            queue->addPoint(0, normalized, pointIndex);
        }
        pendingChanges_.clear();

        int32 channels = (layout_ == ChannelLayout::kStereo) ? 2 : 1;
        stereoScratch_.assign(static_cast<size_t>(channels) * numSamples, 0.0f);
        std::vector<float*> inPtrs(static_cast<size_t>(channels));
        std::vector<float*> outPtrs(static_cast<size_t>(channels));
        for (int32 ch = 0; ch < channels; ++ch) {
            inPtrs[static_cast<size_t>(ch)] = stereoScratch_.data() + static_cast<size_t>(ch) * numSamples;
            outPtrs[static_cast<size_t>(ch)] = inPtrs[static_cast<size_t>(ch)];
            for (std::size_t i = 0; i < numSamples; ++i) inPtrs[static_cast<size_t>(ch)][i] = buffer[i];
        }

        AudioBusBuffers inBus;
        inBus.numChannels = channels;
        inBus.channelBuffers32 = inPtrs.data();
        AudioBusBuffers outBus;
        outBus.numChannels = channels;
        outBus.channelBuffers32 = outPtrs.data();

        ProcessData data;
        data.processMode = kRealtime;
        data.symbolicSampleSize = kSample32;
        data.numSamples = static_cast<int32>(numSamples);
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = &inBus;
        data.outputs = &outBus;
        data.inputParameterChanges = &parameterChanges_;

        processor_->process(data);

        if (channels == 1) {
            for (std::size_t i = 0; i < numSamples; ++i) buffer[i] = outPtrs[0][i];
        } else {
            for (std::size_t i = 0; i < numSamples; ++i) buffer[i] = 0.5f * (outPtrs[0][i] + outPtrs[1][i]);
        }
    }

    DlHandle module_;
    IPtr<IPluginFactory> factory_;
    IPtr<HostApplication> hostApplication_;
    IPtr<ComponentHandler> componentHandler_;
    IPtr<IComponent> component_;
    IPtr<IAudioProcessor> processor_;
    IPtr<IEditController> controller_;
    bool controllerIsSeparate_ = false;
    bool componentInitialized_ = false;
    bool controllerInitialized_ = false;
    bool activated_ = false;
    bool processingActive_ = false;

    ChannelLayout layout_ = ChannelLayout::kMono;
    std::vector<ParameterDescriptor> cachedParameters_;
    std::set<ParamID> knownParamIds_;
    std::vector<std::pair<ParamID, ParamValue>> pendingChanges_;
    ParameterChanges parameterChanges_;
    std::vector<float> stereoScratch_;
};

}  // namespace

std::unique_ptr<IHostedPlugin> loadVst3Plugin(const std::string& bundlePath) {
    return std::make_unique<Vst3PluginHost>(bundlePath);
}

}  // namespace audio_engine
