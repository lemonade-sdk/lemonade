
# Build

```sh
./build-oci-image
```

# Running

## Vulkan

```sh
# Only for rootless containers on SELinux systems
sudo setsebool -P container_use_dri_devices=on

podman run --publish 8000:8000 --volume $HOST_PATH:/srv/lemonade:z --device /dev/dri --rm localhost/lemonade-ai
```

## ROCm

```sh
# Only for rootless containers on SELinux systems
sudo setsebool -P container_use_dri_devices=on
sudo setsebool -P container_use_devices=on

podman run --publish 8000:8000 --volume $HOST_PATH:/srv/lemonade:z --device /dev/kfd --device /dev/dri --rm localhost/lemonade-ai
```
