import requests
import time
import sys


def get_metrics(url):
    try:
        response = requests.get(url)
        response.raise_for_status()
        return response.text
    except Exception as e:
        return f"Error: {e}"


def main():
    url = "http://localhost:13305/metrics"
    if len(sys.argv) > 1:
        url = sys.argv[1]

    print(f"Monitoring metrics from {url}...")
    try:
        while True:
            metrics = get_metrics(url)
            print("\n" + "=" * 40)
            print(f"Time: {time.strftime('%Y-%m-%d %H:%M:%S')}")
            print("=" * 40)

            # Print only lemonade_ metrics for brevity
            for line in metrics.splitlines():
                if line.startswith("lemonade_") and not line.startswith(
                    "lemonade_backend_"
                ):
                    print(line)

            time.sleep(2)
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
