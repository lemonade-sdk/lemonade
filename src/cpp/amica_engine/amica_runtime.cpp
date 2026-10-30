#include "amica_runtime.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>

namespace amica_engine {

using json = nlohmann::json;

void log(const std::string& message) {
    std::cerr << "[amica-engine] " << message << "\n";
    std::cerr.flush();
}

double milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::string gib(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f GiB", static_cast<double>(bytes) / (1ull << 30));
    return text;
}

void check_hr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char text[192];
        std::snprintf(text, sizeof(text), "%s failed: HRESULT 0x%08X", what, static_cast<unsigned>(hr));
        throw std::runtime_error(text);
    }
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string narrow(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(size, 1)) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    std::uint32_t exponent = (h >> 10) & 0x1F;
    std::uint32_t mantissa = h & 0x3FF;
    std::uint32_t bits = 0;
    if (exponent == 0x1F) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else if (exponent != 0) {
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    } else if (mantissa != 0) {
        exponent = 113;
        while ((mantissa & 0x400) == 0) {
            mantissa <<= 1;
            --exponent;
        }
        bits = sign | (exponent << 23) | ((mantissa & 0x3FF) << 13);
    } else {
        bits = sign;
    }
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Rounds to nearest even, as NumPy's float32-to-float16 cast does.
std::uint16_t float_to_half(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000u);
    const std::uint32_t magnitude = bits & 0x7FFFFFFFu;
    if (magnitude >= 0x7F800000u) {
        return static_cast<std::uint16_t>(sign | 0x7C00u | (magnitude > 0x7F800000u ? 0x200u : 0u));
    }
    if (magnitude >= 0x477FF000u) {  // rounds to beyond the largest half
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    }
    if (magnitude < 0x38800000u) {  // a half subnormal or zero
        const float scaled = std::fabs(value) * 16777216.0f;  // 2^24: units of the smallest subnormal
        return static_cast<std::uint16_t>(sign | static_cast<std::uint16_t>(std::nearbyint(scaled)));
    }
    const std::uint32_t rounded = magnitude + 0xFFFu + ((magnitude >> 13) & 1u);
    return static_cast<std::uint16_t>(sign | ((rounded - 0x38000000u) >> 13));
}

std::size_t element_bytes(std::int32_t element_type) {
    switch (element_type) {
        case AMICA_ELEMENT_FP16:
        case AMICA_ELEMENT_BF16:
        case AMICA_ELEMENT_INT16:
        case AMICA_ELEMENT_UINT16: return 2;
        case AMICA_ELEMENT_FP32:
        case AMICA_ELEMENT_INT32:
        case AMICA_ELEMENT_UINT32: return 4;
        case AMICA_ELEMENT_FP64:
        case AMICA_ELEMENT_INT64:
        case AMICA_ELEMENT_UINT64: return 8;
        default: return 0;
    }
}

const char* gfx_arch_of(UINT pci_device_id) {
    struct Entry {
        UINT device_id;
        const char* arch;
    };
    static constexpr Entry table[] = {
        {0x73A8, "gfx1100"}, {0x7448, "gfx1100"}, {0x7449, "gfx1100"}, {0x744A, "gfx1100"},
        {0x744B, "gfx1100"}, {0x744C, "gfx1100"}, {0x745E, "gfx1100"}, {0x73C4, "gfx1101"},
        {0x73C5, "gfx1101"}, {0x73C8, "gfx1101"}, {0x7460, "gfx1101"}, {0x7461, "gfx1101"},
        {0x7470, "gfx1101"}, {0x7478, "gfx1101"}, {0x747E, "gfx1101"}, {0x73F0, "gfx1102"},
        {0x7480, "gfx1102"}, {0x7481, "gfx1102"}, {0x7483, "gfx1102"}, {0x7487, "gfx1102"},
        {0x7489, "gfx1102"}, {0x748B, "gfx1102"}, {0x7499, "gfx1102"}, {0x15BF, "gfx1103"},
        {0x15C8, "gfx1103"}, {0x1900, "gfx1103"}, {0x1901, "gfx1103"}, {0x150E, "gfx1150"},
        {0x1586, "gfx1151"}, {0x1114, "gfx1152"}, {0x1902, "gfx1153"}, {0x7590, "gfx1200"},
        {0x7550, "gfx1201"}, {0x7551, "gfx1201"},
    };
    for (const auto& entry : table) {
        if (entry.device_id == pci_device_id) {
            return entry.arch;
        }
    }
    return nullptr;
}

SafeTensors::SafeTensors(const MappedFile& file) {
    std::uint64_t header_size = 0;
    if (file.size() < sizeof(header_size)) {
        throw std::runtime_error("not a safetensors file");
    }
    std::memcpy(&header_size, file.data(), sizeof(header_size));
    if (header_size > file.size() - sizeof(header_size)) {
        throw std::runtime_error("not a safetensors file");
    }
    const auto header = json::parse(std::string_view(reinterpret_cast<const char*>(file.data()) + sizeof(header_size),
                                                     static_cast<std::size_t>(header_size)));
    const std::uint64_t data_begin = sizeof(header_size) + header_size;
    for (const auto& [name, entry] : header.items()) {
        if (name == "__metadata__") {
            continue;
        }
        const auto begin = entry.at("data_offsets").at(0).get<std::uint64_t>();
        const auto end = entry.at("data_offsets").at(1).get<std::uint64_t>();
        if (end < begin || end > file.size() - data_begin) {
            throw std::runtime_error("safetensors tensor " + name + " lies outside the file");
        }
        Tensor tensor;
        tensor.dtype = entry.at("dtype").get<std::string>();
        tensor.shape = entry.at("shape").get<std::vector<std::uint64_t>>();
        tensor.data = file.data() + data_begin + begin;
        tensor.size = static_cast<std::size_t>(end - begin);
        tensors_.emplace(name, std::move(tensor));
    }
}

const Tensor& SafeTensors::at(const std::string& name) const {
    const auto found = tensors_.find(name);
    if (found == tensors_.end()) {
        throw std::runtime_error("tensor " + name + " is not in the safetensors file");
    }
    return found->second;
}

Gpu::Gpu(const std::string& adapter_selector, const std::string& arch_override) {
    ComPtr<IDXGIFactory4> factory;
    check_hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

    struct Candidate {
        ComPtr<IDXGIAdapter1> adapter;
        std::string name;
        std::string arch;
        bool uma = true;
    };
    std::vector<Candidate> candidates;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.VendorId != 0x1002 || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        ComPtr<ID3D12Device> probe;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&probe)))) {
            continue;
        }
        D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
        probe->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture));
        const char* arch = gfx_arch_of(desc.DeviceId);
        candidates.push_back({adapter, narrow(desc.Description), arch != nullptr ? arch : "",
                              architecture.UMA != FALSE});
    }
    if (candidates.empty()) {
        throw std::runtime_error("no AMD D3D12 adapter was found");
    }

    const Candidate* chosen = nullptr;
    if (!adapter_selector.empty()) {
        const bool is_index = std::all_of(adapter_selector.begin(), adapter_selector.end(),
                                          [](unsigned char c) { return std::isdigit(c) != 0; });
        if (is_index) {
            const auto index = std::stoul(adapter_selector);
            if (index >= candidates.size()) {
                throw std::runtime_error("--adapter " + adapter_selector + " is out of range; " +
                                         std::to_string(candidates.size()) + " AMD adapter(s) found");
            }
            chosen = &candidates[index];
        } else {
            std::string wanted = adapter_selector;
            std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const auto& candidate : candidates) {
                std::string name = candidate.name;
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name.find(wanted) != std::string::npos) {
                    chosen = &candidate;
                    break;
                }
            }
            if (chosen == nullptr) {
                throw std::runtime_error("no AMD adapter matches --adapter " + adapter_selector);
            }
        }
    } else {
        // Like amica-driver: a supported discrete adapter first, so a mixed
        // APU + dGPU system does not land on the slower integrated GPU.
        for (const bool want_discrete : {true, false}) {
            for (const auto& candidate : candidates) {
                if (candidate.uma != want_discrete && !candidate.arch.empty()) {
                    chosen = &candidate;
                    break;
                }
            }
            if (chosen != nullptr) break;
        }
        if (chosen == nullptr) {
            chosen = &candidates.front();
        }
    }

    check_hr(D3D12CreateDevice(chosen->adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)),
             "D3D12CreateDevice");
    chosen->adapter.As(&adapter_);
    adapter_name_ = chosen->name;
    uma_ = chosen->uma;
    gfx_arch_ = arch_override.empty() ? chosen->arch : arch_override;

    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    check_hr(device_->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue_)), "CreateCommandQueue");
    check_hr(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

Gpu::~Gpu() {
    if (event_ != nullptr) CloseHandle(event_);
}

std::uint64_t Gpu::local_memory_budget() const {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!adapter_ || FAILED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        return 0;
    }
    return info.Budget > info.CurrentUsage ? info.Budget - info.CurrentUsage : 0;
}

ComPtr<ID3D12Resource> Gpu::create_buffer(std::uint64_t size, D3D12_HEAP_TYPE heap) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (heap == D3D12_HEAP_TYPE_DEFAULT) {
        flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    } else if (heap == D3D12_HEAP_TYPE_UPLOAD) {
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    } else {
        state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    return create(size, properties, flags, state);
}

ComPtr<ID3D12Resource> Gpu::create_system_buffer(std::uint64_t size) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    return create(size, properties, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

ComPtr<ID3D12Resource> Gpu::create_tiled_system_buffer(std::uint64_t size, const std::uint8_t* data,
                                                       std::vector<ComPtr<ID3D12Heap>>& heaps) {
    const std::uint64_t reserved_size = (size + kTileBytes - 1) / kTileBytes * kTileBytes;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = reserved_size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> reserved;
    check_hr(device_->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&reserved)),
             "CreateReservedResource");

    for (std::uint64_t offset = 0; offset < reserved_size; offset += kSystemHeapBytes) {
        const std::uint64_t heap_size = std::min(kSystemHeapBytes, reserved_size - offset);
        D3D12_HEAP_DESC heap_desc{};
        heap_desc.SizeInBytes = heap_size;
        heap_desc.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
        heap_desc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
        heap_desc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        ComPtr<ID3D12Heap> heap;
        check_hr(device_->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)), "CreateHeap");

        D3D12_TILED_RESOURCE_COORDINATE start{};
        start.X = static_cast<UINT>(offset / kTileBytes);
        D3D12_TILE_REGION_SIZE region{};
        region.NumTiles = static_cast<UINT>(heap_size / kTileBytes);
        const D3D12_TILE_RANGE_FLAGS range_flags = D3D12_TILE_RANGE_FLAG_NONE;
        const UINT heap_start = 0;
        const UINT range_tiles = region.NumTiles;
        queue_->UpdateTileMappings(reserved.Get(), 1, &start, &region, heap.Get(), 1, &range_flags, &heap_start,
                                   &range_tiles, D3D12_TILE_MAPPING_FLAG_NONE);

        // The CPU writes the heap through a placed buffer of its own; the
        // reserved range maps the same memory.
        D3D12_RESOURCE_DESC window_desc = desc;
        window_desc.Width = heap_size;
        ComPtr<ID3D12Resource> window;
        check_hr(device_->CreatePlacedResource(heap.Get(), 0, &window_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               nullptr, IID_PPV_ARGS(&window)),
                 "CreatePlacedResource");
        void* mapped = nullptr;
        check_hr(window->Map(0, nullptr, &mapped), "Map");
        const std::uint64_t bytes = offset < size ? std::min(heap_size, size - offset) : 0;
        std::memcpy(mapped, data + offset, static_cast<std::size_t>(bytes));
        window->Unmap(0, nullptr);
        // Release each copied piece of the source right away; a tensor this
        // large would otherwise hold tens of GiB of file pages in the working set.
        VirtualUnlock(const_cast<std::uint8_t*>(data + offset), static_cast<SIZE_T>(bytes));
        heaps.push_back(heap);
    }
    return reserved;
}

ID3D12GraphicsCommandList* Gpu::begin_list() {
    if (used_ == lists_.size()) {
        Recording recording;
        check_hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&recording.allocator)),
                 "CreateCommandAllocator");
        check_hr(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, recording.allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&recording.list)),
                 "CreateCommandList");
        check_hr(recording.list->Close(), "ID3D12GraphicsCommandList::Close");
        lists_.push_back(std::move(recording));
    }
    auto& recording = lists_[used_++];
    check_hr(recording.allocator->Reset(), "ID3D12CommandAllocator::Reset");
    check_hr(recording.list->Reset(recording.allocator.Get(), nullptr), "ID3D12GraphicsCommandList::Reset");
    current_ = recording.list.Get();
    uav_barrier();
    return current_;
}

void Gpu::uav_barrier() {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = nullptr;
    current_->ResourceBarrier(1, &barrier);
}

void Gpu::transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    current_->ResourceBarrier(1, &barrier);
}

void Gpu::copy_into(ID3D12Resource* destination, std::uint64_t destination_offset, ID3D12Resource* source,
                    std::uint64_t source_offset, std::uint64_t size) {
    transition(destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    current_->CopyBufferRegion(destination, destination_offset, source, source_offset, size);
    transition(destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void Gpu::copy_between(ID3D12Resource* destination, std::uint64_t destination_offset, ID3D12Resource* source,
                       std::uint64_t source_offset, std::uint64_t size) {
    transition(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_into(destination, destination_offset, source, source_offset, size);
    transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void Gpu::read_back(ID3D12Resource* readback, ID3D12Resource* source, std::uint64_t offset, std::uint64_t size) {
    transition(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    current_->CopyBufferRegion(readback, 0, source, offset, size);
    transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void Gpu::submit_and_wait() {
    for (std::size_t i = 0; i < used_; ++i) {
        check_hr(lists_[i].list->Close(), "ID3D12GraphicsCommandList::Close");
        ID3D12CommandList* lists[] = {lists_[i].list.Get()};
        queue_->ExecuteCommandLists(1, lists);
    }
    used_ = 0;
    current_ = nullptr;
    check_hr(queue_->Signal(fence_.Get(), ++fence_value_), "ID3D12CommandQueue::Signal");
    check_hr(fence_->SetEventOnCompletion(fence_value_, event_), "ID3D12Fence::SetEventOnCompletion");
    WaitForSingleObject(event_, INFINITE);
    check_hr(device_->GetDeviceRemovedReason(), "GPU execution (device removed)");
}

ComPtr<ID3D12Resource> Gpu::create(std::uint64_t size, const D3D12_HEAP_PROPERTIES& properties,
                                   D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<std::uint64_t>(size, 256);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    check_hr(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                              IID_PPV_ARGS(&resource)),
             "CreateCommittedResource");
    return resource;
}

Amica::Amica() {
    if (amica_create_factory(AMICA_API_VERSION, &factory) != AMICA_OK) {
        throw std::runtime_error("amica_create_factory failed: incompatible amica.dll");
    }
    check(factory->get_diagnostics(&diagnostics), "get_diagnostics");
    diagnostics->set_error_callback(on_error, this);
    amica::version_desc version;
    if (diagnostics->get_version_info(&version) == AMICA_OK) {
        log(std::string("AMICA ") + version.library_version + " (API " + std::to_string(version.api_version) + ", " +
            version.cgc_ir_release + ", CGC dialect " + version.cgc_dialect_version + ")");
    }
    check(factory->create_compiler(&compiler), "create_compiler");
    check(factory->create_execution(&execution), "create_execution");
}

Amica::~Amica() {
    if (diagnostics) diagnostics->set_error_callback(nullptr, nullptr);
}

void Amica::check(amica_status status, const char* what) {
    if (status == AMICA_OK) {
        return;
    }
    std::string text = std::string(what) + " failed: " + amica::status_name(status);
    if (!last_error_.empty()) {
        text += " (" + last_error_ + ")";
    }
    last_error_.clear();
    throw amica::error(status, text);
}

void AMICA_CALL Amica::on_error(void* user_data, amica_status, const char*, const char* message) {
    static_cast<Amica*>(user_data)->last_error_ = message != nullptr ? message : "";
}

void HostInput::write(std::size_t index, std::int64_t value) {
    if (width == sizeof(std::int32_t)) {
        if (value < (std::numeric_limits<std::int32_t>::min)() || value > (std::numeric_limits<std::int32_t>::max)()) {
            throw std::runtime_error("value " + std::to_string(value) + " does not fit an int32 input");
        }
        const auto narrowed = static_cast<std::int32_t>(value);
        std::memcpy(mapped + index * width, &narrowed, sizeof(narrowed));
    } else {
        std::memcpy(mapped + index * width, &value, sizeof(value));
    }
}

Runtime::Runtime(const std::string& adapter, const std::string& arch) : gpu_(adapter, arch) {
    log("GPU: " + gpu_.adapter_name() + " (" + (gpu_.gfx_arch().empty() ? "arch auto" : gpu_.gfx_arch()) +
        (gpu_.uma() ? ", unified memory)" : ", discrete)"));
    amica_ = std::make_unique<Amica>();
    const amica_device_desc device_desc{AMICA_RUNTIME_DX, gpu_.device(), nullptr};
    amica_->check(amica_->execution->create_device(&device_desc, &device_), "create_device");
    amica_->check(device_->create_pso_cache(&pso_cache_), "create_pso_cache");
}

Resources& Runtime::open_resources(const std::string& path) {
    auto& entry = resources_[path];
    if (!entry) {
        entry = std::make_unique<Resources>(path);
    }
    return *entry;
}

Graph Runtime::compile(const std::string& label, std::string_view ir, Resources* resources,
                       amica::compile_options options) {
    const auto started = std::chrono::steady_clock::now();
    amica::device_info device;
    device.gfx_arch = gpu_.gfx_arch().empty() ? nullptr : gpu_.gfx_arch().c_str();
    options.device_info = &device;
    amica::graph_desc desc;
    desc.ir = ir.data();
    desc.ir_size = ir.size();
    desc.options = &options;
    if (resources != nullptr) {
        desc.resources = resources->file.data();
        desc.resources_size = resources->file.size();
        desc.resources_format = AMICA_COMPILE_RESOURCES_SAFETENSORS;
    }

    Graph graph;
    graph.label = label;
    graph.resources = resources;
    amica_->check(amica_->compiler->compile_graph(&desc, &graph.graph), "compile_graph");
    amica_->check(graph.graph->get_buffer_requirements(&graph.requirements), "get_buffer_requirements");
    const amica_kernel_descriptor* kernels = nullptr;
    std::size_t count = 0;
    amica_->check(graph.graph->get_kernel_descriptors(&kernels, &count), "get_kernel_descriptors");
    for (std::size_t first = 0; first < count;) {
        std::size_t end = first;
        std::uint64_t workgroups = 0;
        while (end < count && end - first < kMaxDispatchesPerList) {
            const std::uint64_t groups =
                std::uint64_t{kernels[end].dispatch_x} * kernels[end].dispatch_y * kernels[end].dispatch_z;
            if (end > first && workgroups + groups > kMaxWorkgroupsPerList) break;
            workgroups += groups;
            ++end;
        }
        graph.ranges.emplace_back(first, end - first);
        first = end;
    }
    amica_->check(amica_->execution->create_pso_list(graph.graph.get(), pso_cache_.get(), nullptr, &graph.psos),
                  "create_pso_list");
    graph.input_buffers.resize(graph.requirements.num_inputs);
    graph.output_buffers.resize(graph.requirements.num_outputs);
    graph.inputs.assign(graph.requirements.num_inputs, 0);
    graph.outputs.assign(graph.requirements.num_outputs, 0);
    graph.weights.assign(graph.requirements.num_weights, 0);
    if (graph.requirements.num_weights != 0 && resources == nullptr) {
        throw std::runtime_error(label + " has external weights but no resources");
    }
    log(label + ": " + std::to_string(count) + " kernels, " + std::to_string(graph.requirements.num_inputs) +
        " inputs, " + std::to_string(graph.requirements.num_outputs) + " outputs, " +
        std::to_string(graph.requirements.num_weights) + " external weights, compiled in " +
        std::to_string(static_cast<int>(milliseconds_since(started))) + " ms");
    return graph;
}

void Runtime::upload_weights(const std::vector<Graph*>& graphs, std::int64_t vram_budget_mib) {
    struct Weight {
        Resources* resources;
        std::string name;
        std::uint64_t size;
    };
    std::map<std::pair<Resources*, std::string>, Weight> unique;
    for (Graph* graph : graphs) {
        for (std::uint32_t i = 0; i < graph->requirements.num_weights; ++i) {
            const auto& weight = graph->requirements.weights[i];
            // AMICA saturates the size of a weight of 4 GiB or more at
            // UINT32_MAX; such a weight is the whole tensor, as amica-driver binds it.
            std::uint64_t size = weight.size_bytes;
            if (size == std::numeric_limits<std::uint32_t>::max()) {
                size = std::max(size, graph->resources->tensors.at(weight.name).size);
            }
            auto& entry = unique[{graph->resources, weight.name}];
            entry = {graph->resources, weight.name, std::max(entry.size, size)};
        }
    }
    if (unique.empty()) {
        return;
    }

    std::vector<const Weight*> order;
    for (const auto& [key, weight] : unique) order.push_back(&weight);
    std::sort(order.begin(), order.end(), [](const Weight* a, const Weight* b) { return a->size < b->size; });
    const std::uint64_t budget = vram_budget_mib < 0 ? std::numeric_limits<std::uint64_t>::max()
                                                     : static_cast<std::uint64_t>(vram_budget_mib) << 20;

    const auto started = std::chrono::steady_clock::now();
    ComPtr<ID3D12Resource> staging;
    std::uint8_t* staging_mapped = nullptr;
    std::map<std::pair<Resources*, std::string>, ComPtr<ID3D12Resource>> buffers;
    for (const Weight* weight : order) {
        const auto& tensor = weight->resources->tensors.at(weight->name);
        if (tensor.size < weight->size) {
            throw std::runtime_error("external weight " + weight->name + " needs " + std::to_string(weight->size) +
                                     " bytes; the tensor has " + std::to_string(tensor.size));
        }
        ComPtr<ID3D12Resource> buffer;
        if (weights_vram_bytes_ + weight->size <= budget) {
            buffer = gpu_.create_buffer(weight->size);
            if (!staging) {
                staging = gpu_.create_buffer(kUploadChunkBytes, D3D12_HEAP_TYPE_UPLOAD);
                void* mapped = nullptr;
                check_hr(staging->Map(0, nullptr, &mapped), "Map");
                staging_mapped = static_cast<std::uint8_t*>(mapped);
            }
            for (std::uint64_t offset = 0; offset < weight->size; offset += kUploadChunkBytes) {
                const std::uint64_t chunk = std::min(kUploadChunkBytes, weight->size - offset);
                std::memcpy(staging_mapped, tensor.data + offset, static_cast<std::size_t>(chunk));
                gpu_.begin_list();
                gpu_.copy_into(buffer.Get(), offset, staging.Get(), 0, chunk);
                gpu_.submit_and_wait();
            }
            weights_vram_bytes_ += weight->size;
        } else if (weight->size > kSystemHeapBytes) {
            buffer = gpu_.create_tiled_system_buffer(weight->size, tensor.data, system_heaps_);
            weights_system_bytes_ += weight->size;
        } else {
            buffer = gpu_.create_system_buffer(weight->size);
            void* mapped = nullptr;
            check_hr(buffer->Map(0, nullptr, &mapped), "Map");
            std::memcpy(mapped, tensor.data, static_cast<std::size_t>(weight->size));
            buffer->Unmap(0, nullptr);
            weights_system_bytes_ += weight->size;
        }
        // The mapped source pages stay in the working set after the copy, so a
        // model placed in system memory would otherwise occupy host memory twice.
        // VirtualUnlock on unlocked pages releases them from the working set.
        VirtualUnlock(const_cast<std::uint8_t*>(tensor.data), static_cast<SIZE_T>(weight->size));
        buffers[{weight->resources, weight->name}] = buffer;
        external_weights_.push_back(buffer);
    }
    for (Graph* graph : graphs) {
        for (std::uint32_t i = 0; i < graph->requirements.num_weights; ++i) {
            graph->weights[i] =
                buffers.at({graph->resources, graph->requirements.weights[i].name})->GetGPUVirtualAddress();
        }
    }
    log("external weights: " + std::to_string(unique.size()) + " tensors, " + gib(weights_vram_bytes_) +
        " in VRAM, " + gib(weights_system_bytes_) + " in system memory, uploaded in " +
        std::to_string(static_cast<int>(milliseconds_since(started))) + " ms");
}

HostInput Runtime::host_input(const amica_io_desc& io, bool integer) {
    HostInput input;
    input.size = io.size_bytes;
    if (integer) {
        input.width = element_bytes(io.element_type);
        if ((io.element_type != AMICA_ELEMENT_INT32 && io.element_type != AMICA_ELEMENT_INT64) ||
            io.size_bytes % input.width != 0) {
            throw std::runtime_error(std::string("input ") + io.name + " is not int32/int64 storage");
        }
    }
    input.buffer = gpu_.create_buffer(io.size_bytes);
    input.upload = gpu_.create_buffer(io.size_bytes, D3D12_HEAP_TYPE_UPLOAD);
    void* mapped = nullptr;
    check_hr(input.upload->Map(0, nullptr, &mapped), "Map");
    input.mapped = static_cast<std::uint8_t*>(mapped);
    return input;
}

void Runtime::upload(HostInput& input) {
    if (input) {
        gpu_.copy_into(input.buffer.Get(), 0, input.upload.Get(), 0, input.size);
    }
}

void Runtime::initialize_graphs(const std::vector<Graph*>& graphs) {
    std::uint64_t scratch = 0;
    std::uint64_t init_scratch = 0;
    for (Graph* graph : graphs) {
        scratch = std::max(scratch, graph->requirements.inference_temporary_size);
        init_scratch = std::max(init_scratch, graph->requirements.init_temporary_size);
    }
    if (scratch != 0) scratch_ = gpu_.create_buffer(scratch);
    if (init_scratch != 0) init_scratch_ = gpu_.create_buffer(init_scratch);

    for (Graph* graph : graphs) {
        if (graph->requirements.persistent_size != 0) {
            graph->persistent = gpu_.create_buffer(graph->requirements.persistent_size);
        }
        gpu_.begin_list();
        amica::graph_init_desc init;
        init.device = device_.get();
        init.cmd_list = gpu_.list();
        init.graph = graph->graph.get();
        init.persistent_va = graph->persistent ? graph->persistent->GetGPUVirtualAddress() : 0;
        init.init_scratch_va = init_scratch_ ? init_scratch_->GetGPUVirtualAddress() : 0;
        amica_->check(amica_->execution->init_graph(&init), "init_graph");
        gpu_.submit_and_wait();
    }
}

void Runtime::encode(Graph& graph) {
    const std::uint64_t persistent = graph.persistent ? graph.persistent->GetGPUVirtualAddress() : 0;
    for (const auto& [first, count] : graph.ranges) {
        if (first != 0 || gpu_.list() == nullptr) {
            gpu_.begin_list();
        }
        amica::pso_list_encode_desc desc;
        desc.cmd_list = gpu_.list();
        desc.pso_list = graph.psos.get();
        desc.persistent_va = persistent;
        desc.inputs = graph.inputs.data();
        desc.num_inputs = graph.inputs.size();
        desc.outputs = graph.outputs.data();
        desc.num_outputs = graph.outputs.size();
        desc.weights = graph.weights.data();
        desc.num_weights = graph.weights.size();
        desc.scratch_va = scratch_ ? scratch_->GetGPUVirtualAddress() : 0;
        desc.first_kernel = first;
        desc.kernel_count = count;
        amica_->check(amica_->execution->encode_pso_list(&desc), "encode_pso_list");
    }
    gpu_.uav_barrier();
}

bool Runtime::device_removed() {
    return device_->get_device_removed_reason() == AMICA_ERROR_DEVICE_REMOVED;
}

}  // namespace amica_engine
