# RadicaGame RDF Decode Notes

Most `RadicaGame\data\system\*.rdf` files are obfuscated XML:

- Header/magic: `FF 00 FF AC EB 96 C4 2A`
- Payload: XOR each byte after the header with repeating key `44 32 6C E5 88 79 7F 95`
- Text encoding: `cp1252`
- Decoded terminator: many files end with small binary tail bytes after the
  final XML tag; strip them before XML parsing

`station.rdf` does not use this header. It is base64 text, then the same XOR key
with a shifted key offset. `tools/decode_rdf.py` detects and decodes that wrapper
too.

Some decoded files contain game text/control bytes that are not strict XML, but
the same byte decoder still emits their readable text.

Decode a file:

```powershell
py .\tools\decode_rdf.py "C:\Users\jLynx\Documents\U.B. Funkeys\RadicaGame\data\system\funkeys.rdf" -o .\decoded\funkeys.xml
```

Decode every RDF file in a folder:

```powershell
py .\tools\decode_rdf.py "C:\Users\jLynx\Documents\U.B. Funkeys\RadicaGame\data\system" --output-dir .\decoded_rdf\system
```

Regenerate the funkey ID/name exports:

```powershell
py .\tools\decode_rdf.py "C:\Users\jLynx\Documents\U.B. Funkeys\RadicaGame\data\system\funkeys.rdf" --funkeys-csv .\docs\funkeys-rdf.csv --funkeys-tsv .\docs\funkeys-rdf.tsv
```

The checked-in export from `funkeys.rdf` contains 176 rows, 176 unique IDs, and
78 unique display names.
