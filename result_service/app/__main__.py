from __future__ import annotations

import argparse

from .config import ServiceConfig
from .server import serve


def main() -> None:
    parser = argparse.ArgumentParser(description="Test tracking result and AI service")
    parser.add_argument("--config", help="JSON service configuration")
    args = parser.parse_args()
    serve(ServiceConfig.from_file(args.config))


if __name__ == "__main__":
    main()

