# Check and visualize page tables over time
# Input: Directory containing files names pagemap_<time>.txt
#       - pagemap_<time>.txt contains the page table mappings at a given time during execution
#       - 3-Column format: <virtual address start> <physical address start> <contiguous size>
import pandas as pd
import numpy as np
import sys
import os

# Check whether page table mappings change over time
def check_page_table_mappings(dir):
    # Get all files in the directory and iterate in time order
    files = [f for f in os.listdir(dir) if f.startswith("pagemap_")]
    files.sort(key=lambda x: int(x.split('_')[1].split('.')[0]))

    # Store mappings of each virtual page
    mappings = {}
    changes = {}
    total = 0

    # Iterate over each file
    for file in files:
        # Read the file into a DataFrame (hex integers must be converted to int)
        time = int(file.split('_')[1].split('.')[0])
        df = pd.read_csv(os.path.join(dir, file))
        df['VPN'] = df['VPN'].apply(lambda x: int(str(x), 16))
        df['PFN'] = df['PFN'].apply(lambda x: int(str(x), 16))
        df['Size'] = df['Size'].apply(lambda x: int(str(x), 16))

        # Iterate over each row in the DataFrame
        changed = 0
        for _, row in df.iterrows():
            virtual_start = row['VPN']
            physical_start = row['PFN']
            size = row['Size']

            # Map each page
            for i in range(size):
                vpn = virtual_start + i
                pfn = physical_start + i

                # Check mapping has changed
                if vpn in mappings:
                    if mappings[vpn] != pfn:
                        changed += 1
                mappings[vpn] = pfn
        changes[time] = changed
        total += changed

        # Uncomment to print changes at each time step
        # if changed > 0:
        #     print(f"Time {time}: {changed} changes")
    print(f"Page Mappings Changed: {total}")
    return changes

# Get directory from command line argument
if len(sys.argv) != 2:
    print("Usage: python check_ptables.py <directory>")
    sys.exit(1)
directory = sys.argv[1]
if not os.path.isdir(directory):
    print(f"Error: {directory} is not a valid directory")
    sys.exit(1)

# Check page table mappings
changes = check_page_table_mappings(directory)