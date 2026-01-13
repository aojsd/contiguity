# Update contiguity git repository to a certain branch
# Usage: ./select_branch.sh <branch_name> <remote_hosts (can be multiple)>
if [ "$#" -lt 1 ]; then
    echo "Usage: ./select_branch.sh <branch_name> <remote_hosts (can be multiple)>"
    exit 1
fi

# Loop through hosts
for var in "${@:2}"; do
    ssh $var "cd ~/ISCA_2025_results/contiguity && git pull"
    # Update PiTracer repository
    ssh $var "cd ~/ISCA_2025_results/contiguity && git checkout $1 && make" &
done

wait $(jobs -p)