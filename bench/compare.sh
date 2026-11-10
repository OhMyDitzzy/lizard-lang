#!/usr/bin/env bash
# Prints a before/after table in Markdown for two TSV files of the same kind.
#
#   bench/compare.sh A.tsv B.tsv
#
# Time files come from bench/measure.sh, instruction files from bench/profile.sh. The kind is read
# from the header line. A is "before", B is "after".
#
# Time files: median, change in percent, and the peak memory. A change smaller than twice the
# combined deviation of the two medians is marked `noise`. A geometric mean of B/A per engine
# closes the table (all benchmarks, noise included).
# Instruction files: Ir and the change in percent. Ir does not vary from run to run, so every
# difference counts and nothing is marked noise.
#
# Environment:
#   LABEL_A, LABEL_B  column names (default before, after)
#   SECTION           time or memory, for a time file: only that table (default both in one)
#
# Needs only bash and awk.

set -u

A=${1:-}
B=${2:-}
[ -r "$A" ] && [ -r "$B" ] || {
    echo "usage: compare.sh A.tsv B.tsv" >&2
    exit 2
}
LABEL_A=${LABEL_A:-before}
LABEL_B=${LABEL_B:-after}
SECTION=${SECTION:-all}

kind=$(head -n 1 "$A" | awk -F'\t' '{ print ($3 == "min_ms") ? "time" : ($3 == "ir") ? "ir" : "?" }')
[ "$kind" != "?" ] || {
    echo "compare: $A is neither a time nor an instruction TSV" >&2
    exit 2
}

awk -F'\t' -v kind="$kind" -v la="$LABEL_A" -v lb="$LABEL_B" -v section="$SECTION" '
function num(s) { return (s ~ /^[0-9.]+$/) }
function pct(a, b) { return (b - a) * 100 / a }
function fmtp(p) { return sprintf("%+.1f%%", p) }
function fmt(x) { return (x == int(x)) ? sprintf("%d", x) : sprintf("%.1f", x) }
function commas(n,   s, r) {
    s = sprintf("%d", n); r = ""
    while (length(s) > 3) { r = "," substr(s, length(s) - 2) r; s = substr(s, 1, length(s) - 3) }
    return s r
}
FNR == 1 { next }
FNR == NR {
    key = $1 SUBSEP $2
    if (!(key in seen)) { seen[key] = 1; order[++n] = key; name[key] = $1; eng[key] = $2 }
    if (kind == "time") { amed[key] = $4; adev[key] = $5; amem[key] = $6; ast[key] = $7 }
    else { air[key] = $3 }
    next
}
{
    key = $1 SUBSEP $2
    if (!(key in seen)) { seen[key] = 1; order[++n] = key; name[key] = $1; eng[key] = $2 }
    if (kind == "time") { bmed[key] = $4; bdev[key] = $5; bmem[key] = $6; bst[key] = $7 }
    else { bir[key] = $3 }
}
END {
    if (kind == "ir") {
        print "| benchmark | engine | " la " Ir | " lb " Ir | change |"
        print "| --- | --- | ---: | ---: | ---: |"
        for (i = 1; i <= n; i++) {
            k = order[i]
            if (!num(air[k]) || !num(bir[k]) || air[k] == 0) {
                printf "| %s | %s | %s | %s | n/a |\n", name[k], eng[k], (num(air[k]) ? commas(air[k]) : "-"), (num(bir[k]) ? commas(bir[k]) : "-")
                continue
            }
            p = pct(air[k], bir[k]); lr[eng[k]] += log(bir[k] / air[k]); cnt[eng[k]]++
            if (!(eng[k] in eseen)) { eseen[eng[k]] = 1; elist[++ne] = eng[k] }
            printf "| %s | %s | %s | %s | %s |\n", name[k], eng[k], commas(air[k]), commas(bir[k]), fmtp(p)
        }
        print ""
        for (j = 1; j <= ne; j++) {
            e = elist[j]
            printf "Geometric mean of Ir, %s: %s over %d benchmarks\n\n", e, fmtp((exp(lr[e] / cnt[e]) - 1) * 100), cnt[e]
        }
        exit
    }
    mem_only = (section == "memory"); time_only = (section == "time")
    if (mem_only) {
        print "| benchmark | engine | " la " peak KB | " lb " peak KB | change |"
        print "| --- | --- | ---: | ---: | ---: |"
    } else if (time_only) {
        print "| benchmark | engine | " la " ms | " lb " ms | change | |"
        print "| --- | --- | ---: | ---: | ---: | --- |"
    } else {
        print "| benchmark | engine | " la " ms | " lb " ms | change | | " la " KB | " lb " KB | change |"
        print "| --- | --- | ---: | ---: | ---: | --- | ---: | ---: | ---: |"
    }
    for (i = 1; i <= n; i++) {
        k = order[i]
        if (!(eng[k] in eseen)) { eseen[eng[k]] = 1; elist[++ne] = eng[k] }
        ok = num(amed[k]) && num(bmed[k]) && amed[k] > 0
        note = ""; ch = "n/a"; ma = "-"; mb = "-"
        if (ok) {
            d = bmed[k] - amed[k]
            comb2 = adev[k] ^ 2 + bdev[k] ^ 2
            ch = fmtp(pct(amed[k], bmed[k]))
            if (d * d <= 4 * comb2) note = "noise"
            lr[eng[k]] += log(bmed[k] / amed[k]); cnt[eng[k]]++
        } else if (ast[k] != "ok" || bst[k] != "ok") {
            note = (ast[k] != "ok" ? la ": " ast[k] : "") (bst[k] != "ok" ? " " lb ": " bst[k] : "")
        }
        mch = "n/a"
        if (num(amem[k]) && num(bmem[k]) && amem[k] > 0) mch = fmtp(pct(amem[k], bmem[k]))
        if (num(amem[k])) ma = amem[k]
        if (num(bmem[k])) mb = bmem[k]
        ta = num(amed[k]) ? fmt(amed[k]) : "-"; tb = num(bmed[k]) ? fmt(bmed[k]) : "-"
        if (mem_only) printf "| %s | %s | %s | %s | %s |\n", name[k], eng[k], ma, mb, mch
        else if (time_only) printf "| %s | %s | %s | %s | %s | %s |\n", name[k], eng[k], ta, tb, ch, note
        else printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n", name[k], eng[k], ta, tb, ch, note, ma, mb, mch
    }
    if (!mem_only) {
        print ""
        for (j = 1; j <= ne; j++) {
            e = elist[j]
            if (cnt[e] > 0)
                printf "Geometric mean of time, %s: %s over %d benchmarks\n\n", e, fmtp((exp(lr[e] / cnt[e]) - 1) * 100), cnt[e]
        }
    }
}
' "$A" "$B"
