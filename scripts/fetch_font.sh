#!/usr/bin/env bash
# Downloads Noto Sans JP into assets/fonts/.
# Run once. Requires curl and unzip.
set -euo pipefail

mkdir -p assets/fonts
cd assets/fonts

URL="https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/Japanese/NotoSansJP-Regular.otf"
OUT="NotoSansJP-Regular.ttf"

if [ -f "$OUT" ]; then
    echo "Already present: $OUT"
    exit 0
fi

echo "Downloading Noto Sans JP ..."
# Convert OTF→TTF is not necessary; raylib reads OTF fine. Save as .otf.
curl -fL -o NotoSansJP-Regular.otf "$URL"
echo "Done. File: assets/fonts/NotoSansJP-Regular.otf"
echo "Add this path or update fonts.c if you renamed it."
