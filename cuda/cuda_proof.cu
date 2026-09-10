// Real accelerator work performed behind the Ensemble Fabric authority
// boundary.  This translation unit deliberately contains only device code and
// plain host entry points: it makes no authority decision of its own, and it
// never includes the runtime, so the runtime stays free of any CUDA dependency.
#include <cuda_runtime.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

__global__ void scale_kernel(const float* input, float* output, int count, float factor) {
  const int index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index < count) {
    output[index] = input[index] * factor;
  }
}

void write_detail(char* detail, std::size_t detail_bytes, const char* text) {
  if (detail == nullptr || detail_bytes == 0u) {
    return;
  }
  std::snprintf(detail, detail_bytes, "%s", text);
}

}  // namespace

extern "C" int ef_cuda_accelerator_proof(char* detail, std::size_t detail_bytes) {
  int device_count = 0;
  cudaError_t status = cudaGetDeviceCount(&device_count);
  if (status != cudaSuccess) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "cudaGetDeviceCount: %s", cudaGetErrorString(status));
    write_detail(detail, detail_bytes, buffer);
    return 1;
  }
  if (device_count == 0) {
    write_detail(detail, detail_bytes, "no CUDA device is available");
    return 2;
  }
  cudaDeviceProp properties{};
  status = cudaGetDeviceProperties(&properties, 0);
  if (status != cudaSuccess) {
    write_detail(detail, detail_bytes, "cudaGetDeviceProperties failed");
    return 3;
  }

  constexpr int kCount = 4096;
  const std::size_t bytes = static_cast<std::size_t>(kCount) * sizeof(float);
  float* host_input = static_cast<float*>(std::malloc(bytes));
  float* host_output = static_cast<float*>(std::malloc(bytes));
  if (host_input == nullptr || host_output == nullptr) {
    std::free(host_input);
    std::free(host_output);
    write_detail(detail, detail_bytes, "host allocation failed");
    return 4;
  }
  for (int index = 0; index < kCount; ++index) {
    host_input[index] = static_cast<float>(index) * 0.5f;
    host_output[index] = 0.0f;
  }

  float* device_input = nullptr;
  float* device_output = nullptr;
  int result = 0;
  char buffer[320];
  do {
    status = cudaMalloc(reinterpret_cast<void**>(&device_input), bytes);
    if (status != cudaSuccess) {
      std::snprintf(buffer, sizeof(buffer), "cudaMalloc: %s", cudaGetErrorString(status));
      write_detail(detail, detail_bytes, buffer);
      result = 5;
      break;
    }
    status = cudaMalloc(reinterpret_cast<void**>(&device_output), bytes);
    if (status != cudaSuccess) {
      std::snprintf(buffer, sizeof(buffer), "cudaMalloc: %s", cudaGetErrorString(status));
      write_detail(detail, detail_bytes, buffer);
      result = 6;
      break;
    }
    status = cudaMemcpy(device_input, host_input, bytes, cudaMemcpyHostToDevice);
    if (status != cudaSuccess) {
      write_detail(detail, detail_bytes, "host to device copy failed");
      result = 7;
      break;
    }
    const int threads = 256;
    const int blocks = (kCount + threads - 1) / threads;
    scale_kernel<<<blocks, threads>>>(device_input, device_output, kCount, 3.0f);
    status = cudaGetLastError();
    if (status != cudaSuccess) {
      write_detail(detail, detail_bytes, "kernel launch failed");
      result = 8;
      break;
    }
    status = cudaDeviceSynchronize();
    if (status != cudaSuccess) {
      write_detail(detail, detail_bytes, "device synchronize failed");
      result = 9;
      break;
    }
    status = cudaMemcpy(host_output, device_output, bytes, cudaMemcpyDeviceToHost);
    if (status != cudaSuccess) {
      write_detail(detail, detail_bytes, "device to host copy failed");
      result = 10;
      break;
    }
    for (int index = 0; index < kCount; ++index) {
      const float expected = host_input[index] * 3.0f;
      if (host_output[index] != expected) {
        std::snprintf(buffer, sizeof(buffer), "CPU reference parity failed at index %d", index);
        write_detail(detail, detail_bytes, buffer);
        result = 11;
        break;
      }
    }
  } while (false);

  if (device_input != nullptr) {
    cudaFree(device_input);
  }
  if (device_output != nullptr) {
    cudaFree(device_output);
  }
  std::free(host_input);
  std::free(host_output);

  if (result == 0) {
    std::snprintf(buffer, sizeof(buffer),
                  "real execution on %s (compute capability %d.%d), %d elements verified "
                  "against a CPU reference",
                  properties.name, properties.major, properties.minor, kCount);
    write_detail(detail, detail_bytes, buffer);
  }
  return result;
}

extern "C" int ef_cuda_device_memory_baseline(char* detail, std::size_t detail_bytes) {
  std::size_t free_bytes = 0;
  std::size_t total_bytes = 0;
  cudaError_t status = cudaMemGetInfo(&free_bytes, &total_bytes);
  if (status != cudaSuccess) {
    write_detail(detail, detail_bytes, "cudaMemGetInfo failed");
    return 1;
  }
  const std::size_t probe_bytes = free_bytes > (1u << 20) ? (1u << 20) : free_bytes;
  void* probe = nullptr;
  status = cudaMalloc(&probe, probe_bytes);
  if (status != cudaSuccess) {
    write_detail(detail, detail_bytes, "probe allocation failed");
    return 2;
  }
  status = cudaFree(probe);
  if (status != cudaSuccess) {
    write_detail(detail, detail_bytes, "probe release failed");
    return 3;
  }
  std::size_t free_after = 0;
  std::size_t total_after = 0;
  status = cudaMemGetInfo(&free_after, &total_after);
  if (status != cudaSuccess) {
    write_detail(detail, detail_bytes, "cudaMemGetInfo after release failed");
    return 4;
  }
  // A leak would leave measurably less memory free than before the probe.  The
  // tolerance absorbs the runtime's own small internal allocations.
  const std::size_t tolerance = 4u << 20;
  if (free_after + tolerance < free_bytes) {
    std::snprintf(detail, detail_bytes,
                  "device memory did not return to baseline: %zu MiB free before, %zu MiB after",
                  free_bytes >> 20, free_after >> 20);
    return 5;
  }
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer),
                "device memory returned to baseline (%zu MiB free before probe, %zu MiB after "
                "release)",
                free_bytes >> 20, free_after >> 20);
  write_detail(detail, detail_bytes, buffer);
  return 0;
}
