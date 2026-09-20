"""Explicit legacy text codecs; never transform protocol framing or binary data."""
import codecs

# Deliberately exclude stateful/escape and DBCS encodings: native agents parse
# byte strings and some keyboard/path operations assume single-byte characters.
# UTF-8 remains an explicit option for compatible custom agents/applications.
_NAMES = ["ascii", "utf-8", "latin-1", "mac_roman", "mac_latin2", "mac_cyrillic",
          "mac_greek", "mac_iceland", "mac_turkish"]
_NAMES += [f"cp{n}" for n in (437, 720, 737, 775, 850, 852, 855, 857, 858,
                              860, 861, 862, 863, 865, 866, 869, *range(1250, 1259))]
_NAMES += [f"iso8859-{n}" for n in (*range(1, 12), *range(13, 17))]
TEXT_ENCODINGS = frozenset(codecs.lookup(name).name for name in _NAMES)
OUTPUT_ENCODINGS = TEXT_ENCODINGS | {"utf-8-sig", "utf-16", "utf-16-le", "utf-16-be"}


def normalize_encoding(name: str, *, output: bool = False) -> str:
    """Validate names without reflecting arbitrary configuration text in errors."""
    try:
        canonical = codecs.lookup(name).name
    except (LookupError, TypeError):
        raise ValueError("unknown text encoding") from None
    if canonical not in (OUTPUT_ENCODINGS if output else TEXT_ENCODINGS):
        raise ValueError("unsupported text encoding")
    if not output and bytes(range(128)).decode("ascii").encode(canonical) != bytes(range(128)):
        raise ValueError("command encoding must preserve ASCII protocol bytes")
    return canonical


def decode_text(data: bytes, encoding: str, context: str) -> str:
    try:
        return data.decode(encoding, "strict")
    except UnicodeDecodeError as e:
        # Do not expose raw data: a command or registry value may be sensitive.
        raise ValueError(f"{context} is not valid {encoding} at byte {e.start}; check the configured encoding") from None
