import sys
def parse_time_to_seconds(elapsed_time):
    """
    Convert elapsed time in the format [dd-]hh:mm:ss to seconds.

    Args:
        elapsed_time (str): The elapsed time string, e.g., '2-01:23:45' or '01:23:45'.

    Returns:
        int: The total time in seconds.
    """
    days, hours, minutes, seconds = 0, 0, 0, 0

    if '-' in elapsed_time:
        days_part, time_part = elapsed_time.split('-')
        days = int(days_part)
        elapsed_time = time_part

    time_parts = elapsed_time.split(':')

    if len(time_parts) == 3:
        hours, minutes, seconds = map(int, time_parts)
    elif len(time_parts) == 2:
        minutes, seconds = map(int, time_parts)
    else:
        raise ValueError("Invalid time format")

    total_seconds = (days * 86400) + (hours * 3600) + (minutes * 60) + seconds
    return total_seconds

# Example usage
elapsed_time = sys.argv[1]
seconds = parse_time_to_seconds(elapsed_time)
print(f"{seconds}")
