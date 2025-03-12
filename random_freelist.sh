# Check if argument is 1 or 0
# Usage: ./random_freelist.sh <0 or 1>
if [ "$#" -ne 1 ]; then
    echo "Usage: ./random_freelist.sh <0 or 1>"
    exit 1
fi

# Add the kernel boot parameter page_alloc.shuffle=1 to enable randomization of the free list
if [ "$1" -eq 1 ]; then
    echo "Enabling randomization of the free list"
    sudo grubby --args="page_alloc.shuffle=1" --update-kernel=ALL
else
    echo "Disabling randomization of the free list"
    sudo grubby --remove-args="page_alloc.shuffle" --update-kernel=ALL
fi