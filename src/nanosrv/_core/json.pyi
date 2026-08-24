"""JSON parsing utilities"""

def number(json: bytes | bytearray | memoryview | str, path: str) -> float | None:
    """Extract a number from JSON at the given path."""

def boolean(json: bytes | bytearray | memoryview | str, path: str) -> bool | None:
    """Extract a boolean from JSON at the given path."""

def integer(json: bytes | bytearray | memoryview | str, path: str) -> int | None:
    """Extract an integer from JSON at the given path."""

def string(json: bytes | bytearray | memoryview | str, path: str) -> str | None:
    """Extract a string from JSON at the given path."""
