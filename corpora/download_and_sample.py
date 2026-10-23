import urllib.request
import zstandard as zstd
import struct
import os
import sys

def download_and_sample(url, name, num_keys=10_000_000):
    initial_path = f"data/{name}_initial.txt"
    insert_path = f"data/{name}_insert.txt"
    if os.path.exists(initial_path) and os.path.exists(insert_path):
        print(f"[{name}] Output files already exist. Skipping.")
        return

    print(f"[{name}] Starting download and decompression...")
    req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
    
    dctx = zstd.ZstdDecompressor()
    decompressor = dctx.decompressobj()
    
    buffer = bytearray()
    needed_bytes = 8 + num_keys * 8
    
    bytes_downloaded = 0
    with urllib.request.urlopen(req) as resp:
        while len(buffer) < needed_bytes:
            chunk = resp.read(256 * 1024) # 256 KB chunks
            if not chunk:
                break
            bytes_downloaded += len(chunk)
            decompressed = decompressor.decompress(chunk)
            buffer.extend(decompressed)
            # Print progress every 10 MB downloaded
            if bytes_downloaded % (10 * 1024 * 1024) < 256 * 1024:
                print(f"[{name}] Downloaded: {bytes_downloaded / (1024*1024):.1f} MB, decompressed buffer size: {len(buffer) / (1024*1024):.1f} MB")
                
    print(f"[{name}] Decompression finished. Buffer size: {len(buffer)} bytes.")
    
    # Parse header
    total_elements = struct.unpack('<Q', buffer[:8])[0]
    print(f"[{name}] Total elements in full dataset: {total_elements}")
    
    # Parse keys
    actual_keys_bytes = buffer[8:8 + num_keys * 8]
    actual_read = len(actual_keys_bytes) // 8
    print(f"[{name}] Unpacking {actual_read} keys...")
    
    keys = struct.unpack(f'<{actual_read}Q', actual_keys_bytes)
    
    print(f"[{name}] Converting {actual_read} keys to fixed-width string format (e.g. f\"/{{k:020d}}\")...")
    # Using 20-digit zero padding to support all 64-bit uint64_t keys while preserving sorting order
    string_keys = [f"/{k:020d}" for k in keys]
    
    # Split into 80% initial and 20% insert
    split = int(actual_read * 0.8)
    initial_keys = string_keys[:split]
    insert_keys = string_keys[split:]
    
    os.makedirs("data", exist_ok=True)
    
    print(f"[{name}] Writing {initial_path}...")
    with open(initial_path, "w", encoding="utf-8") as f:
        for k in initial_keys:
            f.write(k + "\n")
            
    print(f"[{name}] Writing {insert_path}...")
    with open(insert_path, "w", encoding="utf-8") as f:
        for k in insert_keys:
            f.write(k + "\n")
            
    print(f"[{name}] Done! Initial: {len(initial_keys)}, Inserts: {len(insert_keys)}")

def main():
    # Direct download endpoints for Dataverse files:
    # 3860043 is wiki_ts_200M_uint64.zst
    # 3821004 is osm_cellids_200M_uint64.zst
    wiki_url = "https://dataverse.harvard.edu/api/access/datafile/3860043"
    osm_url = "https://dataverse.harvard.edu/api/access/datafile/3821004"
    
    download_and_sample(wiki_url, "wiki_ts")
    download_and_sample(osm_url, "osm_cellids")

if __name__ == "__main__":
    main()
