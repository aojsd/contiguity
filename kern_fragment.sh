# If the first argument is 1, insert the kernel module
# If the first argument is 0, remove the kernel module
# Usage: ./script.sh <insert> <GB to allocate (insert only)> <% immovable (insert only)>
if [ "$#" -lt 1 ]; then
    echo "Usage: ./script.sh <insert> [GB to allocate (insert only)] [% immovable (insert only)]"
    exit 1
fi
if [ "$1" -ne 0 ] && [ "$1" -ne 1 ]; then
    echo "Usage: ./script.sh <insert> [GB to allocate (insert only)] [% immovable (insert only)]"
    exit 1
fi

# Insert
if [ "$1" -eq 1 ]; then
    # Must have only one arg (default parameters), or three args (GB to allocate, % immovable)
    if [ "$#" -ne 1 ] && [ "$#" -ne 3 ]; then
        echo "Usage: ./script.sh <insert> [GB to allocate] [% immovable]"
        exit 1
    fi

    # Default parameters
    if [ "$#" -eq 1 ]; then
        sudo insmod kern_module/alloc_pages_randomize.ko
    
    # Custom parameters
    else
        sudo insmod kern_module/alloc_pages_randomize.ko total_gb=$2 immovable_pct=$3
    fi
fi

# Remove
if [ "$1" -eq 0 ]; then
    sudo rmmod alloc_pages_randomize
fi

