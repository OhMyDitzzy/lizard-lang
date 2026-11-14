#!/usr/bin/env bash
# Puts the data of one bench run into a single Markdown report.
#
#   bench/report.sh DATA_DIR [out.md]
#
# DATA_DIR holds what the bench workflow collects (every part but meta.txt and time-head.tsv is
# optional, and without the base files the report only shows the head):
#
#   meta.txt                  `key: value` lines (date, refs, tool versions, inputs ...)
#   smoke-head.tsv, smoke-base.tsv   bench/measure.sh at SCALE=1000
#   time-head.tsv, time-base.tsv     bench/measure.sh
#   head/, base/              bench/profile.sh out-dirs (instructions.tsv, profile/, warnings.txt)
#   warnings.txt              more warnings, one per line
#
# The report stands alone: a person or an agent can judge a change from it without opening other
# files. It stays under about 200 KB by cutting the function lists (TOPN, default 15, is halved
# until it fits). Default out file: DATA_DIR/bench-report.md. Needs only bash and awk.

set -u

DATA=${1:-}
[ -d "$DATA" ] || {
    echo "usage: report.sh DATA_DIR [out.md]" >&2
    exit 2
}
OUT=${2:-$DATA/bench-report.md}
HERE=$(cd "$(dirname "$0")" && pwd)
MAX_BYTES=${MAX_BYTES:-190000}
topn=${TOPN:-15}

have() { [ -s "$DATA/$1" ]; }
has_base_time=no
have time-base.tsv && has_base_time=yes
has_base_ir=no
have base/instructions.tsv && has_base_ir=yes

# single TSV as a table: time files
time_table() { # file
    awk -F'\t' 'NR == 1 { print "| benchmark | engine | min ms | median ms | dev ms | peak KB | status |"
                          print "| --- | --- | ---: | ---: | ---: | ---: | --- |"; next }
                { printf "| %s | %s | %s | %s | %s | %s | %s |\n", $1, $2, $3, $4, $5, $6, $7 }' "$1"
}

ir_table() { # file
    awk -F'\t' 'function commas(s,   r) { r = ""; while (length(s) > 3) { r = "," substr(s, length(s) - 2) r; s = substr(s, 1, length(s) - 3) } return s r }
                NR == 1 { print "| benchmark | engine | Ir |"; print "| --- | --- | ---: |"; next }
                { printf "| %s | %s | %s |\n", $1, $2, ($3 ~ /^[0-9]+$/ ? commas($3) : $3) }' "$1"
}

smoke_section() {
    local f
    for f in head base; do
        have "smoke-$f.tsv" || continue
        local pass fail
        pass=$(awk -F'\t' 'NR > 1 && $7 == "ok"' "$DATA/smoke-$f.tsv" | wc -l | tr -d ' ')
        fail=$(awk -F'\t' 'NR > 1 && $7 != "ok" && $7 !~ /^skipped/' "$DATA/smoke-$f.tsv" | wc -l | tr -d ' ')
        echo "**$f**: $pass passed, $fail failed."
        echo
    done
    # one table with a column per ref
    if have smoke-head.tsv; then
        if have smoke-base.tsv; then
            awk -F'\t' 'FNR == 1 { next }
                FNR == NR { b[$1 SUBSEP $2] = $7; next }
                { k = $1 SUBSEP $2; if (!(k in b)) b[k] = "-"; printf "| %s | %s | %s | %s |\n", $1, $2, b[k], $7 }' \
                "$DATA/smoke-base.tsv" "$DATA/smoke-head.tsv" |
                { echo "| benchmark | engine | base | head |"; echo "| --- | --- | --- | --- |"; cat; }
        else
            awk -F'\t' 'NR == 1 { print "| benchmark | engine | head |"; print "| --- | --- | --- |"; next }
                { printf "| %s | %s | %s |\n", $1, $2, $7 }' "$DATA/smoke-head.tsv"
        fi
    else
        echo "No smoke results."
    fi
}

# functions_head: one small table per benchmark and engine from head/profile
functions_section() {
    local f name eng
    [ -d "$DATA/head/profile" ] || {
        echo "Not collected (profile=false)."
        return
    }
    for f in "$DATA"/head/profile/*.txt; do
        [ -e "$f" ] || continue
        name=$(basename "$f" .txt)
        eng=${name##*.}
        name=${name%.*}
        echo "#### $name ($eng)"
        echo
        echo "| % Ir | function |"
        echo "| ---: | --- |"
        awk -F'\t' -v top="$topn" '/^#/ { next } NR > 1 && ++n <= top { printf "| %s | `%s` |\n", $1, $3 }' "$f"
        echo
        if [ -r "$DATA/base/profile/$name.$eng.txt" ]; then
            awk -F'\t' -v top=5 '
                FNR == 1 { next }
                FNR == NR { b[$3] = $1 + 0; next }
                { h[$3] = $1 + 0 }
                END {
                    for (k in b) seen[k] = 1
                    for (k in h) seen[k] = 1
                    for (k in seen) { d = h[k] - b[k]; if (d < 0) d = -d; delta[k] = d }
                    for (i = 1; i <= top; i++) {
                        best = ""; bd = -1
                        for (k in delta) if (delta[k] > bd) { bd = delta[k]; best = k }
                        if (best == "" || bd < 0.05) break
                        printf "| `%s` | %.2f%% | %.2f%% | %+.2f |\n", best, b[best], h[best], h[best] - b[best]
                        delete delta[best]
                    }
                }' "$DATA/base/profile/$name.$eng.txt" "$f" |
                {
                    first=$(cat)
                    if [ -n "$first" ]; then
                        echo "Functions whose share of Ir changed most (base to head):"
                        echo
                        echo "| function | base % Ir | head % Ir | points |"
                        echo "| --- | ---: | ---: | ---: |"
                        printf '%s\n' "$first"
                        echo
                    fi
                }
        fi
    done
}

# allocations: base against head from allocations.tsv, then the five that allocate most
allocations_section() {
    local head=$DATA/head/allocations.tsv base=$DATA/base/allocations.tsv
    if [ ! -s "$head" ]; then
        echo "Not collected (profile=false)."
        return
    fi
    echo "Allocations of the reference engine at a small size, counted by dhat. Blocks and bytes are totals over the run, peak is what was alive at the most (t-gmax)."
    echo
    if [ -s "$base" ]; then
        echo "| benchmark | blocks base | blocks head | change | bytes base | bytes head | change | peak bytes base | peak bytes head | change | peak blocks base | peak blocks head |"
        echo "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
        awk -F'\t' '
            function pct(a, b) { return (a ~ /^[0-9]+$/ && b ~ /^[0-9]+$/ && a > 0) ? sprintf("%+.1f%%", (b - a) * 100 / a) : "-" }
            FNR == 1 { next }
            FNR == NR { bb[$1] = $2; by[$1] = $3; bp[$1] = $4; bk[$1] = $5; next }
            {
                n = $1
                if (!(n in bb)) { bb[n] = "-"; by[n] = "-"; bp[n] = "-"; bk[n] = "-" }
                printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n", n, bb[n], $2, pct(bb[n], $2), by[n], $3, pct(by[n], $3), bp[n], $4, pct(bp[n], $4), bk[n], $5
            }' "$base" "$head"
    else
        echo "| benchmark | blocks | bytes | peak bytes | peak blocks |"
        echo "| --- | ---: | ---: | ---: | ---: |"
        awk -F'\t' 'NR > 1 { printf "| %s | %s | %s | %s | %s |\n", $1, $2, $3, $4, $5 }' "$head"
    fi
    echo
    echo "The five benchmarks that allocate most bytes (head):"
    echo
    echo "| benchmark | bytes | blocks | peak bytes | peak blocks |"
    echo "| --- | ---: | ---: | ---: | ---: |"
    awk -F'\t' 'NR > 1 && $3 ~ /^[0-9]+$/ { print $3 "\t" $1 "\t" $2 "\t" $4 "\t" $5 }' "$head" | sort -t"$(printf '\t')" -k1,1nr |
        head -n 5 | awk -F'\t' '{ printf "| %s | %s | %s | %s | %s |\n", $2, $1, $3, $4, $5 }'
}

warnings_section() {
    local n=0 w
    emit() { echo "- $*"; n=$((n + 1)); }
    local f
    for f in smoke-head smoke-base time-head time-base; do
        have "$f.tsv" || continue
        while IFS= read -r w; do
            [ -n "$w" ] && emit "$f: $w"
        done < <(awk -F'\t' 'NR > 1 && $7 != "ok" { printf "%s on %s: %s\n", $1, $2, $7 }' "$DATA/$f.tsv")
    done
    for f in head base; do
        have "$f/warnings.txt" || continue
        while IFS= read -r w; do
            [ -n "$w" ] && emit "profile $f: $w"
        done <"$DATA/$f/warnings.txt"
    done
    if have warnings.txt; then
        while IFS= read -r w; do
            [ -n "$w" ] && emit "$w"
        done <"$DATA/warnings.txt"
    fi
    [ "$has_base_time" = yes ] || emit "no base ref: the tables show the head only, with no comparison"
    [ "$n" -gt 0 ] || echo "None."
}

build() {
    echo "# Benchmark report"
    echo
    echo "## Metadata"
    echo
    if have meta.txt; then
        echo '```text'
        cat "$DATA/meta.txt"
        echo '```'
    else
        echo "No metadata."
    fi
    echo
    echo "## Smoke test"
    echo
    echo "Every benchmark at a tiny size on each engine, output compared across engines."
    echo
    smoke_section
    echo
    echo "## Time"
    echo
    if [ "$has_base_time" = yes ]; then
        echo "Median wall time in milliseconds. A change smaller than twice the combined deviation is marked noise."
        echo
        LABEL_A=base LABEL_B=head SECTION=time bash "$HERE/compare.sh" "$DATA/time-base.tsv" "$DATA/time-head.tsv"
    else
        echo "Wall time of the head, in milliseconds."
        echo
        have time-head.tsv && time_table "$DATA/time-head.tsv"
    fi
    echo
    echo "## Instructions (Ir)"
    echo
    if [ "$has_base_ir" = yes ] && have head/instructions.tsv; then
        echo "Instructions counted by callgrind at a small size. They do not vary between runs, so every change is real."
        echo
        LABEL_A=base LABEL_B=head bash "$HERE/compare.sh" "$DATA/base/instructions.tsv" "$DATA/head/instructions.tsv"
    elif have head/instructions.tsv; then
        ir_table "$DATA/head/instructions.tsv"
    else
        echo "Not collected (profile=false)."
    fi
    echo
    echo "## Allocations"
    echo
    allocations_section
    echo
    echo "## Peak memory"
    echo
    if [ "$has_base_time" = yes ]; then
        LABEL_A=base LABEL_B=head SECTION=memory bash "$HERE/compare.sh" "$DATA/time-base.tsv" "$DATA/time-head.tsv"
    elif have time-head.tsv; then
        awk -F'\t' 'NR == 1 { print "| benchmark | engine | peak KB |"; print "| --- | --- | ---: |"; next }
            { printf "| %s | %s | %s |\n", $1, $2, $6 }' "$DATA/time-head.tsv"
    fi
    echo
    echo "## Top functions (head)"
    echo
    functions_section
    echo "## Warnings"
    echo
    warnings_section
}

while :; do
    build >"$OUT"
    size=$(wc -c <"$OUT" | tr -d ' ')
    [ "$size" -le "$MAX_BYTES" ] || [ "$topn" -le 3 ] && break
    topn=$((topn / 2))
    [ "$topn" -ge 3 ] || topn=3
done
if [ "$size" -gt "$MAX_BYTES" ]; then
    echo "report: $OUT is $size bytes, over the limit $MAX_BYTES" >&2
fi
exit 0
