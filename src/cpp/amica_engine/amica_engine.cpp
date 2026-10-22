// amica-engine: keeps an AMICA ChatApp bundle resident on a D3D12 GPU and
// serves greedy generation for amica_server.py.
//
// Protocol (UTF-8 JSON, one object per line):
//   stdin  line 1: the generation plan amica_server.py derives from amica_app.json
//   stdin  then:   {"prompt_ids": [...], "max_new_tokens": N, "stop_ids": [...]}
//   stdout:        "@@amica-engine " + {"ready": ...} once, then per request
//                  {"token": id}... and {"done": true, ...}, or {"error": "..."}
// The prefix exists because amica.dll may write stray lines to stdout.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <amica/amica.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

constexpr const char* kProtocolPrefix = "@@amica-engine ";

// Upper bounds for one command list, as the AMICA API recommends, so a long
// graph never runs as a single submission long enough to trip the GPU timeout.
constexpr std::size_t kMaxDispatchesPerList = 32;
constexpr std::uint64_t kMaxWorkgroupsPerList = 262144;

// Weights placed in VRAM are staged through an upload buffer this large, so a
// model far larger than host memory never needs a second full copy.
constexpr std::uint64_t kUploadChunkBytes = 256ull << 20;

// The largest system-memory weight allocated as one buffer. Some drivers remove
// the device when a single GPU-visible system allocation reaches 4 GiB, so larger
// weights are stitched from heaps of this size behind one reserved address range.
constexpr std::uint64_t kSystemHeapBytes = 2ull << 30;

// D3D12 maps reserved buffers in 64 KiB tiles.
constexpr std::uint64_t kTileBytes = 64ull << 10;

void emit(const json& event) {
    std::cout << kProtocolPrefix << event.dump() << "\n";
    std::cout.flush();
}

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

// A file mapped read-only. compile_graph borrows its resources until the graph's
// final release, so mappings live as long as the engine.
class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
        file_ = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("cannot open " + path);
        }
        LARGE_INTEGER size{};
        GetFileSizeEx(file_, &size);
        size_ = static_cast<std::size_t>(size.QuadPart);
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        data_ = mapping_ != nullptr ? MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0) : nullptr;
        if (data_ == nullptr) {
            throw std::runtime_error("cannot map " + path);
        }
    }
    ~MappedFile() {
        if (data_ != nullptr) UnmapViewOfFile(data_);
        if (mapping_ != nullptr) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const std::uint8_t* data() const { return static_cast<const std::uint8_t*>(data_); }
    std::size_t size() const { return size_; }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    const void* data_ = nullptr;
    std::size_t size_ = 0;
};

struct Tensor {
    std::string dtype;
    std::vector<std::uint64_t> shape;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

class SafeTensors {
public:
    explicit SafeTensors(const MappedFile& file) {
        std::uint64_t header_size = 0;
        if (file.size() < sizeof(header_size)) {
            throw std::runtime_error("not a safetensors file");
        }
        std::memcpy(&header_size, file.data(), sizeof(header_size));
        if (header_size > file.size() - sizeof(header_size)) {
            throw std::runtime_error("not a safetensors file");
        }
        const auto header = json::parse(
            std::string_view(reinterpret_cast<const char*>(file.data()) + sizeof(header_size),
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

    const Tensor& at(const std::string& name) const {
        const auto found = tensors_.find(name);
        if (found == tensors_.end()) {
            throw std::runtime_error("tensor " + name + " is not in the safetensors file");
        }
        return found->second;
    }

private:
    std::map<std::string, Tensor> tensors_;
};

// A safetensors file kept mapped for the engine's lifetime: compile_graph
// borrows it, and external weights are uploaded from it by name.
struct Resources {
    explicit Resources(const std::string& path) : file(path), tensors(file) {}
    MappedFile file;
    SafeTensors tensors;
};

// The token embedding table, read on the CPU one FP16 row at a time.
class EmbeddingTable {
public:
    EmbeddingTable(const std::string& path, const std::string& tensor_name)
        : file_(path), tensors_(file_), tensor_(tensors_.at(tensor_name)) {
        if (tensor_.dtype != "F16" || tensor_.shape.size() != 2 ||
            tensor_.size != tensor_.shape[0] * tensor_.shape[1] * sizeof(std::uint16_t)) {
            throw std::runtime_error("embedding tensor " + tensor_name + " must be a rank-2 F16 matrix");
        }
    }

    std::size_t rows() const { return static_cast<std::size_t>(tensor_.shape[0]); }
    std::size_t columns() const { return static_cast<std::size_t>(tensor_.shape[1]); }

    void write_f32_row(std::size_t token, float* destination) const {
        if (token >= rows()) {
            throw std::runtime_error("token " + std::to_string(token) + " is outside the embedding table");
        }
        const auto* row = tensor_.data + token * columns() * sizeof(std::uint16_t);
        for (std::size_t column = 0; column < columns(); ++column) {
            std::uint16_t value;
            std::memcpy(&value, row + column * sizeof(value), sizeof(value));
            destination[column] = half_to_float(value);
        }
    }

private:
    MappedFile file_;
    SafeTensors tensors_;
    const Tensor& tensor_;
};

// The GPU architecture AMICA compiles for, from the adapter's PCI device ID.
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

// A D3D12 device with one compute queue. Work is recorded into a pool of
// command lists and submitted in order with a single wait at the end.
class Gpu {
public:
    Gpu(const std::string& adapter_selector, const std::string& arch_override) {
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
        adapter_name_ = chosen->name;
        uma_ = chosen->uma;
        gfx_arch_ = arch_override.empty() ? chosen->arch : arch_override;

        D3D12_COMMAND_QUEUE_DESC queue{};
        queue.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        check_hr(device_->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue_)), "CreateCommandQueue");
        check_hr(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    ~Gpu() {
        if (event_ != nullptr) CloseHandle(event_);
    }

    ID3D12Device* device() const { return device_.Get(); }
    const std::string& adapter_name() const { return adapter_name_; }
    const std::string& gfx_arch() const { return gfx_arch_; }
    bool uma() const { return uma_; }

    ComPtr<ID3D12Resource> create_buffer(std::uint64_t size, D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT) {
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

    // A buffer in system memory that the CPU writes directly and kernels read
    // in place, over PCIe on a discrete GPU.
    ComPtr<ID3D12Resource> create_system_buffer(std::uint64_t size) {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_CUSTOM;
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        return create(size, properties, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // A system-memory buffer holding `data`, as one reserved address range backed
    // by heaps of at most kSystemHeapBytes. The heaps must outlive the buffer.
    ComPtr<ID3D12Resource> create_tiled_system_buffer(std::uint64_t size, const std::uint8_t* data,
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
        check_hr(device_->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&reserved)),
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
            queue_->UpdateTileMappings(reserved.Get(), 1, &start, &region, heap.Get(), 1, &range_flags,
                                       &heap_start, &range_tiles, D3D12_TILE_MAPPING_FLAG_NONE);

            // The CPU writes the heap through a placed buffer of its own; the
            // reserved range maps the same memory.
            D3D12_RESOURCE_DESC window_desc = desc;
            window_desc.Width = heap_size;
            ComPtr<ID3D12Resource> window;
            check_hr(device_->CreatePlacedResource(heap.Get(), 0, &window_desc,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                   IID_PPV_ARGS(&window)),
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

    // Starts a new command list that orders after everything recorded before it.
    ID3D12GraphicsCommandList* begin_list() {
        if (used_ == lists_.size()) {
            Recording recording;
            check_hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                     IID_PPV_ARGS(&recording.allocator)),
                     "CreateCommandAllocator");
            check_hr(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, recording.allocator.Get(),
                                                nullptr, IID_PPV_ARGS(&recording.list)),
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

    ID3D12GraphicsCommandList* list() const { return current_; }

    void uav_barrier() {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = nullptr;
        current_->ResourceBarrier(1, &barrier);
    }

    void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        current_->ResourceBarrier(1, &barrier);
    }

    // Copies into a default-heap buffer that otherwise stays in the
    // unordered-access state AMICA requires.
    void copy_into(ID3D12Resource* destination, std::uint64_t destination_offset, ID3D12Resource* source,
                   std::uint64_t source_offset, std::uint64_t size) {
        transition(destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        current_->CopyBufferRegion(destination, destination_offset, source, source_offset, size);
        transition(destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    void copy_between(ID3D12Resource* destination, std::uint64_t destination_offset, ID3D12Resource* source,
                      std::uint64_t source_offset, std::uint64_t size) {
        transition(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        copy_into(destination, destination_offset, source, source_offset, size);
        transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    void read_back(ID3D12Resource* readback, ID3D12Resource* source, std::uint64_t offset, std::uint64_t size) {
        transition(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        current_->CopyBufferRegion(readback, 0, source, offset, size);
        transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Executes every list recorded since the last submit, in order, and waits.
    void submit_and_wait() {
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

private:
    ComPtr<ID3D12Resource> create(std::uint64_t size, const D3D12_HEAP_PROPERTIES& properties,
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

    struct Recording {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
    };

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    std::uint64_t fence_value_ = 0;
    std::vector<Recording> lists_;
    std::size_t used_ = 0;
    ID3D12GraphicsCommandList* current_ = nullptr;
    std::string adapter_name_;
    std::string gfx_arch_;
    bool uma_ = false;
};

// The AMICA objects, with failures turned into exceptions that carry the
// message AMICA's error callback received.
class Amica {
public:
    Amica() {
        if (amica_create_factory(AMICA_API_VERSION, &factory) != AMICA_OK) {
            throw std::runtime_error("amica_create_factory failed: incompatible amica.dll");
        }
        check(factory->get_diagnostics(&diagnostics), "get_diagnostics");
        diagnostics->set_error_callback(on_error, this);
        amica::version_desc version;
        if (diagnostics->get_version_info(&version) == AMICA_OK) {
            log(std::string("AMICA ") + version.library_version + " (API " + std::to_string(version.api_version) +
                ", " + version.cgc_ir_release + ", CGC dialect " + version.cgc_dialect_version + ")");
        }
        check(factory->create_compiler(&compiler), "create_compiler");
        check(factory->create_execution(&execution), "create_execution");
    }

    ~Amica() {
        if (diagnostics) diagnostics->set_error_callback(nullptr, nullptr);
    }

    void check(amica_status status, const char* what) {
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

    amica::interface_ptr<amica_factory> factory;
    amica::interface_ptr<amica_diagnostics> diagnostics;
    amica::interface_ptr<amica_compiler> compiler;
    amica::interface_ptr<amica_execution> execution;

private:
    static void AMICA_CALL on_error(void* user_data, amica_status, const char*, const char* message) {
        static_cast<Amica*>(user_data)->last_error_ = message != nullptr ? message : "";
    }

    std::string last_error_;
};

// One compiled graph with its PSO list, persistent buffer and the GPU
// addresses every encode passes.
struct Graph {
    std::string label;
    Resources* resources = nullptr;
    amica::interface_ptr<amica_graph_interface> graph;
    amica::interface_ptr<amica_pso_list> psos;
    amica::buffer_requirements requirements;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;  // first kernel, count
    ComPtr<ID3D12Resource> persistent;
    std::vector<ComPtr<ID3D12Resource>> input_buffers;
    std::vector<ComPtr<ID3D12Resource>> output_buffers;
    std::vector<std::uint64_t> inputs;
    std::vector<std::uint64_t> outputs;
    std::vector<std::uint64_t> weights;

    int input_index(const std::string& name) const {
        if (name.empty()) return -1;
        for (std::uint32_t i = 0; i < requirements.num_inputs; ++i) {
            if (name == requirements.inputs[i].name) return static_cast<int>(i);
        }
        return -1;
    }

    const amica_io_desc& input(int index) const { return requirements.inputs[index]; }
    const amica_io_desc& output(int index) const { return requirements.outputs[index]; }
};

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

// A default-heap input with a persistently mapped upload twin. Each request
// writes the twin on the CPU and records a copy into the input.
struct HostInput {
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12Resource> upload;
    std::uint8_t* mapped = nullptr;
    std::uint64_t size = 0;
    std::size_t width = 0;  // bytes per integer element; 0 for FP32 embeddings

    explicit operator bool() const { return static_cast<bool>(buffer); }
    std::size_t elements() const { return width == 0 ? 0 : static_cast<std::size_t>(size / width); }

    void write(std::size_t index, std::int64_t value) {
        if (width == sizeof(std::int32_t)) {
            if (value < (std::numeric_limits<std::int32_t>::min)() ||
                value > (std::numeric_limits<std::int32_t>::max)()) {
                throw std::runtime_error("value " + std::to_string(value) + " does not fit an int32 input");
            }
            const auto narrowed = static_cast<std::int32_t>(value);
            std::memcpy(mapped + index * width, &narrowed, sizeof(narrowed));
        } else {
            std::memcpy(mapped + index * width, &value, sizeof(value));
        }
    }

    void fill(std::int64_t value) {
        for (std::size_t i = 0; i < elements(); ++i) write(i, value);
    }
};

// The per-request inputs of a prefill or decoder graph: an embedding row per
// token, or token IDs with an attention mask, plus optional position inputs.
struct PromptInputs {
    HostInput embedding;
    HostInput token;
    HostInput mask;
    HostInput position;
    HostInput sequence;
};

struct PrefillVariant {
    Graph graph;
    std::size_t capacity = 0;
    PromptInputs inputs;
};

struct InputNames {
    std::string embedding;
    std::string token;
    std::string mask;
    std::string position;
    std::string sequence;
};

class Engine {
public:
    Engine(const json& plan, const std::string& adapter, const std::string& arch, std::int64_t weights_vram_mib)
        : gpu_(adapter, arch) {
        const auto started = std::chrono::steady_clock::now();
        log("GPU: " + gpu_.adapter_name() + " (" + (gpu_.gfx_arch().empty() ? "arch auto" : gpu_.gfx_arch()) +
            (gpu_.uma() ? ", unified memory)" : ", discrete)"));

        graph_logits_ = plan.at("logits").get<std::string>() == "graph";
        prompt_right_aligned_ = plan.at("prompt_padding").get<std::string>() == "left";
        external_weights_min_bytes_ = plan.value("external_weights_min_bytes", std::uint64_t{0});
        const auto& names = plan.at("inputs");
        const auto name = [&](const char* key) { return names.value(key, json()).is_string()
                                                            ? names.at(key).get<std::string>()
                                                            : std::string(); };
        prefill_names_ = {name("prefill_embedding"), name("prefill_token"), name("prefill_attention_mask"),
                          name("prefill_position"), name("prefill_sequence")};
        decoder_names_ = {name("decoder_embedding"), name("decoder_token"), name("decoder_attention_mask"),
                          name("decoder_position"), name("decoder_sequence")};
        for (const auto& input : plan.value("zero_initialize", std::vector<std::string>())) {
            zero_initialize_.insert(input);
        }
        const json fills = plan.value("initial_input_fill", json::object());
        for (const auto& [input, value] : fills.items()) {
            initial_fill_[input] = value.get<std::int64_t>();
        }

        if (graph_logits_) {
            vocabulary_ = plan.at("vocab_size").get<std::size_t>();
            pad_token_id_ = plan.value("pad_token_id", std::int64_t{0});
        } else {
            embeddings_ = std::make_unique<EmbeddingTable>(plan.at("embedding").at("file").get<std::string>(),
                                                           plan.at("embedding").at("tensor").get<std::string>());
            vocabulary_ = embeddings_->rows();
            tail_row_offset_ = plan.value("tail_row_offset", 0);
        }

        amica_ = std::make_unique<Amica>();
        amica_->check(amica_->execution->create_pso_cache(gpu_.device(), &pso_cache_), "create_pso_cache");

        Resources& resources = open_resources(plan.at("resources").get<std::string>());
        decoder_ = compile("decoder", plan.at("decoder").get<std::string>(), resources);
        for (const auto& entry : plan.at("prefill")) {
            auto variant = std::make_unique<PrefillVariant>();
            variant->capacity = entry.at("capacity").get<std::size_t>();
            variant->graph = compile("prefill[" + std::to_string(variant->capacity) + "]",
                                     entry.at("graph").get<std::string>(), resources);
            for (const auto& [output, input] : entry.at("cache_map").items()) {
                cache_maps_[variant.get()].emplace_back(output, input.get<std::string>());
            }
            prefill_.push_back(std::move(variant));
        }
        std::sort(prefill_.begin(), prefill_.end(),
                  [](const auto& a, const auto& b) { return a->capacity < b->capacity; });
        if (!graph_logits_) {
            const auto& lm_head = plan.at("lm_head");
            lm_head_ = compile("lm_head", lm_head.at("graph").get<std::string>(),
                               open_resources(lm_head.at("resources").get<std::string>()));
            tail_input_name_ = plan.at("tail_input").get<std::string>();
        }

        upload_weights(weights_vram_mib);
        allocate_decoder();
        for (auto& variant : prefill_) {
            allocate_prefill(*variant);
        }
        if (!graph_logits_) {
            allocate_lm_head(plan.at("lm_head").value("resource_map", json::object()));
        }
        allocate_logits_readback();
        allocate_shared();
        initialize_graphs();

        load_ms_ = milliseconds_since(started);
    }

    json ready_event() const {
        json capacities = json::array();
        for (const auto& variant : prefill_) capacities.push_back(variant->capacity);
        return {{"ready", true},
                {"adapter", gpu_.adapter_name()},
                {"arch", gpu_.gfx_arch()},
                {"prefill_capacities", capacities},
                {"vocabulary", vocabulary_},
                {"weights_vram_bytes", weights_vram_bytes_},
                {"weights_system_bytes", weights_system_bytes_},
                {"load_ms", load_ms_}};
    }

    void generate(const std::vector<std::size_t>& prompt, std::size_t max_new_tokens,
                  const std::set<std::size_t>& stop_ids) {
        if (prompt.empty()) {
            throw std::runtime_error("prompt_ids is empty");
        }
        if (max_new_tokens == 0) {
            throw std::runtime_error("max_new_tokens must be at least 1");
        }
        PrefillVariant* variant = nullptr;
        for (auto& candidate : prefill_) {
            if (candidate->capacity >= prompt.size()) {
                variant = candidate.get();
                break;
            }
        }
        if (variant == nullptr) {
            throw std::runtime_error("the prompt has " + std::to_string(prompt.size()) +
                                     " tokens, more than the largest prefill capacity " +
                                     std::to_string(prefill_.back()->capacity));
        }

        const auto started = std::chrono::steady_clock::now();
        std::size_t token = prefill(*variant, prompt);
        const double ttft_ms = milliseconds_since(started);
        std::vector<std::size_t> generated{token};
        emit({{"token", token}});

        const auto decode_started = std::chrono::steady_clock::now();
        while (generated.size() < max_new_tokens && stop_ids.count(token) == 0) {
            const auto position = static_cast<std::int64_t>(prompt.size() + generated.size() - 1);
            token = decode(token, position);
            generated.push_back(token);
            emit({{"token", token}});
        }
        const double decode_ms = generated.size() > 1 ? milliseconds_since(decode_started) : 0.0;
        const double tps = decode_ms > 0.0 ? (generated.size() - 1) * 1000.0 / decode_ms : 0.0;
        emit({{"done", true},
              {"ids", generated},
              {"ttft_ms", ttft_ms},
              {"decode_ms", decode_ms},
              {"tps", tps}});
    }

    bool device_removed() {
        return amica_->execution->get_device_removed_reason(gpu_.device()) == AMICA_ERROR_DEVICE_REMOVED;
    }

private:
    Resources& open_resources(const std::string& path) {
        auto& entry = resources_[path];
        if (!entry) {
            entry = std::make_unique<Resources>(path);
        }
        return *entry;
    }

    Graph compile(const std::string& label, const std::string& path, Resources& resources) {
        // Logged first: if amica.dll terminates the process while compiling,
        // this names the graph it was compiling.
        log("compiling " + label + " from " + path);
        const auto started = std::chrono::steady_clock::now();
        MappedFile ir(path);
        amica::device_info device;
        device.gfx_arch = gpu_.gfx_arch().empty() ? nullptr : gpu_.gfx_arch().c_str();
        amica::compile_options options;
        options.device_info = &device;
        options.prompt_right_aligned = prompt_right_aligned_ ? 1 : 0;
        options.external_weights_min_bytes = external_weights_min_bytes_;
        amica::graph_desc desc;
        desc.ir = ir.data();
        desc.ir_size = ir.size();
        desc.options = &options;
        desc.resources = resources.file.data();
        desc.resources_size = resources.file.size();
        desc.resources_format = AMICA_COMPILE_RESOURCES_SAFETENSORS;

        Graph graph;
        graph.label = label;
        graph.resources = &resources;
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
        log(label + ": " + std::to_string(count) + " kernels, " + std::to_string(graph.requirements.num_inputs) +
            " inputs, " + std::to_string(graph.requirements.num_outputs) + " outputs, " +
            std::to_string(graph.requirements.num_weights) + " external weights, compiled in " +
            std::to_string(static_cast<int>(milliseconds_since(started))) + " ms");
        return graph;
    }

    std::vector<Graph*> graphs() {
        std::vector<Graph*> result{&decoder_};
        for (auto& variant : prefill_) result.push_back(&variant->graph);
        if (!graph_logits_) result.push_back(&lm_head_);
        return result;
    }

    // Every graph names its external weights by tensor; a tensor two graphs
    // read from the same file is uploaded once and bound to both.
    void upload_weights(std::int64_t vram_budget_mib) {
        struct Weight {
            Resources* resources;
            std::string name;
            std::uint64_t size;
        };
        std::map<std::pair<Resources*, std::string>, Weight> unique;
        for (Graph* graph : graphs()) {
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
                throw std::runtime_error("external weight " + weight->name + " needs " +
                                         std::to_string(weight->size) + " bytes; the tensor has " +
                                         std::to_string(tensor.size));
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
        for (Graph* graph : graphs()) {
            for (std::uint32_t i = 0; i < graph->requirements.num_weights; ++i) {
                graph->weights[i] =
                    buffers.at({graph->resources, graph->requirements.weights[i].name})->GetGPUVirtualAddress();
            }
        }
        log("external weights: " + std::to_string(unique.size()) + " tensors, " + gib(weights_vram_bytes_) +
            " in VRAM, " + gib(weights_system_bytes_) + " in system memory, uploaded in " +
            std::to_string(static_cast<int>(milliseconds_since(started))) + " ms");
    }

    HostInput host_input(const amica_io_desc& io, bool integer) {
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

    void bind_input(Graph& graph, int index, const ComPtr<ID3D12Resource>& buffer) {
        graph.input_buffers[index] = buffer;
        graph.inputs[index] = buffer->GetGPUVirtualAddress();
    }

    void bind_output(Graph& graph, int index, const ComPtr<ID3D12Resource>& buffer) {
        graph.output_buffers[index] = buffer;
        graph.outputs[index] = buffer->GetGPUVirtualAddress();
    }

    // Binds the inputs a request writes; returns their indices.
    std::set<int> bind_prompt_inputs(Graph& graph, const InputNames& names, PromptInputs& inputs) {
        std::set<int> bound;
        const auto bind = [&](const std::string& name, HostInput& input, bool integer, bool required) {
            const int index = graph.input_index(name);
            if (index < 0) {
                if (required) {
                    throw std::runtime_error(graph.label + " input " + name + " was not found");
                }
                return;
            }
            input = host_input(graph.input(index), integer);
            bind_input(graph, index, input.buffer);
            bound.insert(index);
        };
        if (graph_logits_) {
            bind(names.token, inputs.token, true, true);
            bind(names.mask, inputs.mask, true, true);
        } else {
            bind(names.embedding, inputs.embedding, false, true);
        }
        bind(names.position, inputs.position, true, false);
        bind(names.sequence, inputs.sequence, true, false);
        return bound;
    }

    // A buffer shared by prefill and decoder is reset once, under prefill's
    // name, because the manifest's initial_input_fill lists prefill names.
    void add_reset(const ComPtr<ID3D12Resource>& buffer, const amica_io_desc& io) {
        Reset reset{buffer, nullptr, io.size_bytes};
        const auto fill = initial_fill_.find(io.name);
        if (fill != initial_fill_.end()) {
            const std::size_t width = element_bytes(io.element_type);
            if ((io.element_type != AMICA_ELEMENT_INT32 && io.element_type != AMICA_ELEMENT_INT64) ||
                io.size_bytes % width != 0) {
                throw std::runtime_error(std::string("initial_input_fill input ") + io.name +
                                         " is not int32/int64 storage");
            }
            reset.source = gpu_.create_buffer(io.size_bytes, D3D12_HEAP_TYPE_UPLOAD);
            void* mapped = nullptr;
            check_hr(reset.source->Map(0, nullptr, &mapped), "Map");
            HostInput writer;
            writer.mapped = static_cast<std::uint8_t*>(mapped);
            writer.size = io.size_bytes;
            writer.width = width;
            writer.fill(fill->second);
            reset.source->Unmap(0, nullptr);
        } else {
            zero_size_ = std::max(zero_size_, io.size_bytes);
        }
        resets_[buffer.Get()] = reset;
    }

    void bind_outputs(Graph& graph) {
        for (std::uint32_t o = 0; o < graph.requirements.num_outputs; ++o) {
            if (graph.output_buffers[o]) continue;
            const auto& output = graph.output(static_cast<int>(o));
            bind_output(graph, static_cast<int>(o),
                        output.aliased_input >= 0 ? graph.input_buffers[output.aliased_input]
                                                  : gpu_.create_buffer(output.size_bytes));
        }
    }

    void allocate_decoder() {
        Graph& graph = decoder_;
        bind_prompt_inputs(graph, decoder_names_, decoder_inputs_);
        if (!graph_logits_ && decoder_inputs_.embedding.size != embeddings_->columns() * sizeof(float)) {
            throw std::runtime_error("decoder embedding input does not hold one FP32 embedding row");
        }
        if (graph_logits_ && decoder_inputs_.token.elements() != 1) {
            throw std::runtime_error("decoder token input must hold one token");
        }
        // State inputs; see allocate_prefill for how prefill hands them over.
        for (std::uint32_t i = 0; i < graph.requirements.num_inputs; ++i) {
            if (graph.input_buffers[i]) continue;
            const auto& io = graph.input(static_cast<int>(i));
            const auto buffer = gpu_.create_buffer(io.size_bytes);
            bind_input(graph, static_cast<int>(i), buffer);
            add_reset(buffer, io);
        }
        bind_outputs(graph);
    }

    void allocate_prefill(PrefillVariant& variant) {
        Graph& graph = variant.graph;
        const auto prompt_inputs = bind_prompt_inputs(graph, prefill_names_, variant.inputs);
        const std::size_t capacity = graph_logits_ ? variant.inputs.token.elements()
                                                   : static_cast<std::size_t>(variant.inputs.embedding.size /
                                                                              (embeddings_->columns() * sizeof(float)));
        if (capacity != variant.capacity) {
            throw std::runtime_error(graph.label + " holds " + std::to_string(capacity) +
                                     " prompt tokens, not the planned " + std::to_string(variant.capacity));
        }

        // The cache map names the decoder input each prefill output feeds. An
        // output updated in place shares its buffer with the input it aliases,
        // so that prefill input becomes the decoder's state buffer too.
        for (const auto& [output_name, decoder_input] : cache_maps_[&variant]) {
            int output = -1;
            for (std::uint32_t o = 0; o < graph.requirements.num_outputs; ++o) {
                if (output_name == graph.output(static_cast<int>(o)).name) output = static_cast<int>(o);
            }
            const int target = decoder_.input_index(decoder_input);
            if (output < 0 || target < 0) {
                throw std::runtime_error(graph.label + " cache map " + output_name + "=" + decoder_input +
                                         " names a missing output or decoder input");
            }
            if (graph.output(output).size_bytes != decoder_.input(target).size_bytes) {
                throw std::runtime_error(graph.label + " cache map " + output_name + "=" + decoder_input +
                                         " joins buffers of different sizes");
            }
            const auto& buffer = decoder_.input_buffers[target];
            bind_output(graph, output, buffer);
            const int aliased = graph.output(output).aliased_input;
            if (aliased >= 0) {
                bind_input(graph, aliased, buffer);
            }
        }
        for (std::uint32_t i = 0; i < graph.requirements.num_inputs; ++i) {
            const int index = static_cast<int>(i);
            if (prompt_inputs.count(index) != 0) continue;
            const auto& io = graph.input(index);
            if (!graph.input_buffers[i]) {
                if (zero_initialize_.count(io.name) == 0 && initial_fill_.count(io.name) == 0) {
                    log(std::string("warning: ") + graph.label + " input " + io.name +
                        " has no plan entry; it starts zeroed");
                }
                bind_input(graph, index, gpu_.create_buffer(io.size_bytes));
            }
            add_reset(graph.input_buffers[i], io);
        }
        bind_outputs(graph);
    }

    void allocate_lm_head(const json& resource_map) {
        Graph& graph = lm_head_;
        tail_input_index_ = graph.input_index(tail_input_name_);
        if (tail_input_index_ < 0) {
            throw std::runtime_error("LM head input " + tail_input_name_ + " was not found");
        }
        if (graph.input(tail_input_index_).size_bytes != embeddings_->columns() * sizeof(float)) {
            throw std::runtime_error("LM head input " + tail_input_name_ + " does not hold one FP32 hidden row");
        }
        lm_input_ = gpu_.create_buffer(graph.input(tail_input_index_).size_bytes);
        bind_input(graph, tail_input_index_, lm_input_);

        std::vector<ComPtr<ID3D12Resource>> staging;
        gpu_.begin_list();
        for (const auto& [input_name, tensor_name] : resource_map.items()) {
            const int index = graph.input_index(input_name);
            if (index < 0) {
                throw std::runtime_error("LM head resource map names missing input " + input_name);
            }
            const auto& tensor = graph.resources->tensors.at(tensor_name.get<std::string>());
            if (tensor.size != graph.input(index).size_bytes) {
                throw std::runtime_error("LM head input " + input_name + " takes " +
                                         std::to_string(graph.input(index).size_bytes) + " bytes; tensor " +
                                         tensor_name.get<std::string>() + " has " + std::to_string(tensor.size));
            }
            const auto buffer = gpu_.create_buffer(tensor.size);
            const auto upload = gpu_.create_buffer(tensor.size, D3D12_HEAP_TYPE_UPLOAD);
            void* mapped = nullptr;
            check_hr(upload->Map(0, nullptr, &mapped), "Map");
            std::memcpy(mapped, tensor.data, tensor.size);
            upload->Unmap(0, nullptr);
            gpu_.copy_into(buffer.Get(), 0, upload.Get(), 0, tensor.size);
            staging.push_back(upload);
            bind_input(graph, index, buffer);
        }
        gpu_.submit_and_wait();
        for (std::uint32_t i = 0; i < graph.requirements.num_inputs; ++i) {
            if (!graph.input_buffers[i]) {
                throw std::runtime_error(std::string("LM head input ") + graph.input(static_cast<int>(i)).name +
                                         " is neither the hidden-state input nor in the resource map");
            }
        }
        bind_outputs(graph);
    }

    // The graphs whose output 0 holds logits: the LM head, or prefill and the
    // decoder themselves. Each produces one or more rows of vocabulary logits.
    void allocate_logits_readback() {
        std::vector<Graph*> producers;
        if (graph_logits_) {
            producers.push_back(&decoder_);
            for (auto& variant : prefill_) producers.push_back(&variant->graph);
        } else {
            producers.push_back(&lm_head_);
        }
        std::uint64_t row_bytes = 0;
        for (Graph* graph : producers) {
            const auto& logits = graph->output(0);
            const std::size_t width = element_bytes(logits.element_type);
            if (logits.element_type != AMICA_ELEMENT_FP16 && logits.element_type != AMICA_ELEMENT_FP32) {
                throw std::runtime_error(graph->label + " logits must be FP16 or FP32");
            }
            if (logits.size_bytes == 0 || logits.size_bytes % (vocabulary_ * width) != 0) {
                throw std::runtime_error(graph->label + " output 0 holds " + std::to_string(logits.size_bytes) +
                                         " bytes, not rows of " + std::to_string(vocabulary_) + " logits");
            }
            row_bytes = std::max<std::uint64_t>(row_bytes, vocabulary_ * width);
        }
        logits_readback_ = gpu_.create_buffer(row_bytes, D3D12_HEAP_TYPE_READBACK);
        void* mapped = nullptr;
        check_hr(logits_readback_->Map(0, nullptr, &mapped), "Map");
        logits_mapped_ = static_cast<const std::uint8_t*>(mapped);
    }

    // One inference scratch buffer serves every graph: they run one after
    // another, never concurrently. The zero source resets state per request.
    void allocate_shared() {
        std::uint64_t scratch = 0;
        std::uint64_t init_scratch = 0;
        for (Graph* graph : graphs()) {
            scratch = std::max(scratch, graph->requirements.inference_temporary_size);
            init_scratch = std::max(init_scratch, graph->requirements.init_temporary_size);
        }
        if (scratch != 0) scratch_ = gpu_.create_buffer(scratch);
        if (init_scratch != 0) init_scratch_ = gpu_.create_buffer(init_scratch);
        if (zero_size_ != 0) {
            zero_source_ = gpu_.create_buffer(zero_size_, D3D12_HEAP_TYPE_UPLOAD);
            void* mapped = nullptr;
            check_hr(zero_source_->Map(0, nullptr, &mapped), "Map");
            std::memset(mapped, 0, static_cast<std::size_t>(zero_size_));
            zero_source_->Unmap(0, nullptr);
        }
    }

    void initialize_graphs() {
        for (Graph* graph : graphs()) {
            if (graph->requirements.persistent_size != 0) {
                graph->persistent = gpu_.create_buffer(graph->requirements.persistent_size);
            }
            gpu_.begin_list();
            amica::graph_init_desc init;
            init.device = gpu_.device();
            init.cmd_list = gpu_.list();
            init.graph = graph->graph.get();
            init.persistent_va = graph->persistent ? graph->persistent->GetGPUVirtualAddress() : 0;
            init.init_scratch_va = init_scratch_ ? init_scratch_->GetGPUVirtualAddress() : 0;
            amica_->check(amica_->execution->init_graph(&init), "init_graph");
            gpu_.submit_and_wait();
        }
    }

    void encode(Graph& graph) {
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

    void upload(HostInput& input) {
        if (input) {
            gpu_.copy_into(input.buffer.Get(), 0, input.upload.Get(), 0, input.size);
        }
    }

    void upload(PromptInputs& inputs) {
        upload(inputs.embedding);
        upload(inputs.token);
        upload(inputs.mask);
        upload(inputs.position);
        upload(inputs.sequence);
    }

    // Reads one row of logits from output 0 of graph and returns its argmax.
    std::size_t argmax_logits(Graph& graph, std::uint64_t row) {
        const auto& logits = graph.output(0);
        const std::size_t width = element_bytes(logits.element_type);
        const std::uint64_t row_bytes = vocabulary_ * width;
        if (row >= logits.size_bytes / row_bytes) {
            throw std::runtime_error("the logits row is outside " + graph.label + " output 0");
        }
        gpu_.read_back(logits_readback_.Get(), graph.output_buffers[0].Get(), row * row_bytes, row_bytes);
        gpu_.submit_and_wait();

        float best = -std::numeric_limits<float>::infinity();
        std::size_t best_token = 0;
        for (std::size_t i = 0; i < vocabulary_; ++i) {
            float value;
            if (width == 4) {
                std::memcpy(&value, logits_mapped_ + i * 4, 4);
            } else {
                std::uint16_t half;
                std::memcpy(&half, logits_mapped_ + i * 2, 2);
                value = half_to_float(half);
            }
            if (std::isfinite(value) && value > best) {
                best = value;
                best_token = i;
            }
        }
        if (!std::isfinite(best)) {
            throw std::runtime_error("the logits contain no finite value");
        }
        return best_token;
    }

    std::size_t run_lm_head(std::uint64_t hidden_va) {
        lm_head_.inputs[tail_input_index_] = hidden_va;
        encode(lm_head_);
        return argmax_logits(lm_head_, 0);
    }

    // The output row of the last prompt token in a prefill output with `rows`
    // rows: the final row when prompts are right-aligned (left-padded), else
    // row n-1.
    std::uint64_t last_prompt_row(std::uint64_t rows, std::size_t prompt_tokens, int offset) const {
        const std::int64_t row =
            static_cast<std::int64_t>(prompt_right_aligned_ ? rows - 1 : prompt_tokens - 1) + offset;
        if (row < 0 || static_cast<std::uint64_t>(row) >= rows) {
            throw std::runtime_error("the prefill row of the last prompt token is outside the prefill output");
        }
        return static_cast<std::uint64_t>(row);
    }

    std::size_t prefill(PrefillVariant& variant, const std::vector<std::size_t>& prompt) {
        PromptInputs& inputs = variant.inputs;
        const std::size_t n = prompt.size();
        if (graph_logits_) {
            const std::size_t first = prompt_right_aligned_ ? variant.capacity - n : 0;
            inputs.token.fill(pad_token_id_);
            for (std::size_t i = 0; i < n; ++i) {
                inputs.token.write(first + i, static_cast<std::int64_t>(prompt[i]));
            }
            const std::size_t mask_capacity = inputs.mask.elements();
            if (n > mask_capacity) {
                throw std::runtime_error("the prompt exceeds the prefill attention-mask capacity");
            }
            const std::size_t mask_first = prompt_right_aligned_ ? mask_capacity - n : 0;
            inputs.mask.fill(0);
            for (std::size_t i = 0; i < n; ++i) {
                inputs.mask.write(mask_first + i, 1);
            }
        } else {
            const std::size_t row_bytes = embeddings_->columns() * sizeof(float);
            std::memset(inputs.embedding.mapped, 0, static_cast<std::size_t>(inputs.embedding.size));
            const std::size_t first = prompt_right_aligned_ ? variant.capacity - n : 0;
            for (std::size_t i = 0; i < n; ++i) {
                embeddings_->write_f32_row(prompt[i],
                                           reinterpret_cast<float*>(inputs.embedding.mapped + (first + i) * row_bytes));
            }
        }
        if (inputs.position) inputs.position.write(0, static_cast<std::int64_t>(n) - 1);
        if (inputs.sequence) inputs.sequence.write(0, static_cast<std::int64_t>(n));

        gpu_.begin_list();
        for (const auto& [resource, reset] : resets_) {
            gpu_.copy_into(reset.buffer.Get(), 0, reset.source ? reset.source.Get() : zero_source_.Get(), 0,
                           reset.size);
        }
        upload(inputs);
        gpu_.uav_barrier();
        encode(variant.graph);

        if (graph_logits_) {
            const std::uint64_t rows = variant.graph.output(0).size_bytes /
                                       (vocabulary_ * element_bytes(variant.graph.output(0).element_type));
            return argmax_logits(variant.graph, rows == 1 ? 0 : last_prompt_row(rows, n, 0));
        }
        const std::uint64_t row_bytes = embeddings_->columns() * sizeof(float);
        const std::uint64_t row = last_prompt_row(variant.graph.output(0).size_bytes / row_bytes, n, tail_row_offset_);
        gpu_.copy_between(lm_input_.Get(), 0, variant.graph.output_buffers[0].Get(), row * row_bytes, row_bytes);
        gpu_.uav_barrier();
        return run_lm_head(lm_input_->GetGPUVirtualAddress());
    }

    // Feeds `token` at `position` (zero-based; the prompt fills 0..n-1).
    std::size_t decode(std::size_t token, std::int64_t position) {
        PromptInputs& inputs = decoder_inputs_;
        if (graph_logits_) {
            inputs.token.write(0, static_cast<std::int64_t>(token));
            const auto valid = static_cast<std::size_t>(position + 1);
            if (valid > inputs.mask.elements()) {
                throw std::runtime_error("the sequence exceeds the decoder attention-mask capacity");
            }
            inputs.mask.fill(0);
            for (std::size_t i = 0; i < valid; ++i) {
                inputs.mask.write(i, 1);
            }
        } else {
            embeddings_->write_f32_row(token, reinterpret_cast<float*>(inputs.embedding.mapped));
        }
        if (inputs.position) inputs.position.write(0, position);
        if (inputs.sequence) inputs.sequence.write(0, position + 1);

        gpu_.begin_list();
        upload(inputs);
        gpu_.uav_barrier();
        encode(decoder_);

        if (graph_logits_) {
            const std::uint64_t rows =
                decoder_.output(0).size_bytes / (vocabulary_ * element_bytes(decoder_.output(0).element_type));
            return argmax_logits(decoder_, rows - 1);
        }
        const std::uint64_t row_bytes = embeddings_->columns() * sizeof(float);
        const std::uint64_t rows = decoder_.output(0).size_bytes / row_bytes;
        return run_lm_head(decoder_.output_buffers[0]->GetGPUVirtualAddress() + (rows - 1) * row_bytes);
    }

    struct Reset {
        ComPtr<ID3D12Resource> buffer;
        ComPtr<ID3D12Resource> source;  // null: copy from the shared zero source
        std::uint64_t size = 0;
    };

    Gpu gpu_;
    std::unique_ptr<Amica> amica_;
    amica::interface_ptr<amica_pso_cache> pso_cache_;
    std::map<std::string, std::unique_ptr<Resources>> resources_;
    std::unique_ptr<EmbeddingTable> embeddings_;

    Graph decoder_;
    std::vector<std::unique_ptr<PrefillVariant>> prefill_;
    std::map<const PrefillVariant*, std::vector<std::pair<std::string, std::string>>> cache_maps_;
    Graph lm_head_;

    bool graph_logits_ = false;
    bool prompt_right_aligned_ = true;
    std::uint64_t external_weights_min_bytes_ = 0;
    std::size_t vocabulary_ = 0;
    std::int64_t pad_token_id_ = 0;
    int tail_row_offset_ = 0;
    InputNames prefill_names_;
    InputNames decoder_names_;
    std::string tail_input_name_;
    std::set<std::string> zero_initialize_;
    std::map<std::string, std::int64_t> initial_fill_;

    PromptInputs decoder_inputs_;
    int tail_input_index_ = -1;
    ComPtr<ID3D12Resource> lm_input_;
    ComPtr<ID3D12Resource> logits_readback_;
    const std::uint8_t* logits_mapped_ = nullptr;

    std::vector<ComPtr<ID3D12Resource>> external_weights_;
    std::vector<ComPtr<ID3D12Heap>> system_heaps_;
    std::uint64_t weights_vram_bytes_ = 0;
    std::uint64_t weights_system_bytes_ = 0;
    std::map<ID3D12Resource*, Reset> resets_;
    std::uint64_t zero_size_ = 0;
    ComPtr<ID3D12Resource> zero_source_;
    ComPtr<ID3D12Resource> scratch_;
    ComPtr<ID3D12Resource> init_scratch_;
    double load_ms_ = 0.0;
};

int usage() {
    std::cerr << "usage: amica-engine --amica-dir <AMICA install>\\amica [--adapter <index|name>] "
                 "[--arch gfxNNNN] [--weights-vram-mib N]\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string amica_dir;
    std::string adapter;
    std::string arch;
    std::int64_t weights_vram_mib = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (i + 1 >= argc) return usage();
        const std::string value = argv[++i];
        if (option == "--amica-dir") {
            amica_dir = value;
        } else if (option == "--adapter") {
            adapter = value;
        } else if (option == "--arch") {
            arch = value;
        } else if (option == "--weights-vram-mib") {
            try {
                weights_vram_mib = std::stoll(value);
            } catch (const std::exception&) {
                return usage();
            }
            if (weights_vram_mib < 0) return usage();
        } else {
            std::cerr << "unknown option " << option << "\n";
            return usage();
        }
    }
    if (amica_dir.empty()) return usage();

    std::unique_ptr<Engine> engine;
    try {
        // amica.dll is delay-loaded; it and the DLLs it depends on load from
        // the package's amica folder.
        if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS) ||
            AddDllDirectory(widen(amica_dir).c_str()) == nullptr) {
            throw std::runtime_error("cannot add " + amica_dir + " to the DLL search path");
        }
        if (GetFileAttributesW(widen(amica_dir + "\\amica.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
            throw std::runtime_error("amica.dll was not found in " + amica_dir);
        }
        std::string line;
        if (!std::getline(std::cin, line)) {
            throw std::runtime_error("no generation plan on stdin");
        }
        engine = std::make_unique<Engine>(json::parse(line), adapter, arch, weights_vram_mib);
        emit(engine->ready_event());
    } catch (const std::exception& error) {
        emit({{"error", error.what()}});
        return 1;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        try {
            const auto request = json::parse(line);
            const auto stop = request.value("stop_ids", std::vector<std::size_t>());
            engine->generate(request.at("prompt_ids").get<std::vector<std::size_t>>(),
                             request.at("max_new_tokens").get<std::size_t>(),
                             std::set<std::size_t>(stop.begin(), stop.end()));
        } catch (const std::exception& error) {
            emit({{"error", error.what()}});
            if (engine->device_removed()) {
                log("the GPU device was removed; exiting so Lemonade reloads the model");
                return 3;
            }
        }
    }
    return 0;
}
