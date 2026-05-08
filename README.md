# RAM Buffer Driver

A Linux kernel module that buffers writes in RAM and asynchronously flushes them to disk using a background kernel thread.

## Features

- RAM-based circular buffer
- Async SSD flushing
- Power management support
- Emergency direct-write mode during suspend/hibernate
- Character device interface

## Configuration

| Parameter | Value |
|---|---|
| Block Size | 256 KB |
| Total Blocks | 4096 |
| Total Buffer | ~1 GB |

## Workflow

1. Data is written to `/dev/my_ram_buffer`
2. Stored temporarily in RAM
3. Background thread flushes data to disk
4. On suspend/hibernate, driver switches to emergency mode

## Build

```bash
make
```

## Load Module

```bash
sudo insmod ram_buffer.ko
dmesg | tail
```

## Create Device

```bash
sudo mknod /dev/my_ram_buffer c <major_number> 0
sudo chmod 666 /dev/my_ram_buffer
```

## Test

```bash
echo "hello" > /dev/my_ram_buffer
```

## Unload

```bash
sudo rmmod ram_buffer
```

## Notes

- Uses `vmalloc()` for memory allocation
- Uses mutexes and wait queues for synchronization
- Intended for learning and experimentation

## License

GPL v2
