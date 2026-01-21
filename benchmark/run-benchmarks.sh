#!/bin/bash

# Emacs Rendering Backend Benchmark Runner
# =========================================
# Builds and benchmarks Cairo, Skia Raster, and Skia GL backends.
#
# Usage:
#   ./benchmark/run-benchmarks.sh [options]
#
# Options:
#   --cairo-only      Only benchmark Cairo
#   --skia-only       Only benchmark Skia (both raster and GL)
#   --skip-build      Skip building, use existing binaries
#   --quick           Run quick benchmarks (fewer iterations)
#   --output DIR      Output directory (default: benchmark-results/TIMESTAMP)

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BENCHMARK_EL="$SCRIPT_DIR/emacs-render-benchmark.el"
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
OUTPUT_DIR="${PROJECT_DIR}/benchmark-results/${TIMESTAMP}"
JOBS=$(nproc 2>/dev/null || echo 4)

# Options
RUN_CAIRO=true
RUN_SKIA_RASTER=true
RUN_SKIA_GL=true
SKIP_BUILD=false
QUICK_MODE=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --cairo-only)
            RUN_SKIA_RASTER=false
            RUN_SKIA_GL=false
            shift
            ;;
        --skia-only)
            RUN_CAIRO=false
            shift
            ;;
        --skip-build)
            SKIP_BUILD=true
            shift
            ;;
        --quick)
            QUICK_MODE=true
            shift
            ;;
        --output)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Create output directory
mkdir -p "$OUTPUT_DIR"
log_info "Results will be saved to: $OUTPUT_DIR"

# Save system info
save_system_info() {
    local info_file="$OUTPUT_DIR/system-info.txt"
    {
        echo "Benchmark Run: $TIMESTAMP"
        echo "========================================"
        echo ""
        echo "System Information:"
        echo "  Hostname: $(hostname)"
        echo "  Kernel: $(uname -r)"
        echo "  CPU: $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)"
        echo "  Cores: $(nproc)"
        echo "  Memory: $(free -h | awk '/^Mem:/ {print $2}')"
        echo ""
        echo "GPU Information:"
        if command -v glxinfo &> /dev/null; then
            glxinfo | grep -E "OpenGL (vendor|renderer|version)" | head -3 || true
        else
            echo "  glxinfo not available"
        fi
        echo ""
        echo "Display:"
        echo "  DISPLAY=$DISPLAY"
        echo "  WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-not set}"
        echo ""
    } > "$info_file"
    log_info "System info saved to: $info_file"
}

# Build a specific configuration
build_emacs() {
    local config_name="$1"
    local config_args="$2"
    local build_dir="$PROJECT_DIR/build-$config_name"

    log_info "Building Emacs ($config_name)..."
    log_info "  Configure args: $config_args"

    cd "$PROJECT_DIR"

    # Clean and configure
    if [[ -f Makefile ]]; then
        make distclean 2>/dev/null || true
    fi

    # Run autogen if needed
    if [[ ! -f configure ]]; then
        log_info "Running autogen.sh..."
        ./autogen.sh
    fi

    # Configure
    log_info "Configuring..."
    ./configure $config_args --prefix="$build_dir/install" 2>&1 | tee "$OUTPUT_DIR/configure-$config_name.log"

    # Build
    log_info "Building with $JOBS jobs..."
    make -j$JOBS 2>&1 | tee "$OUTPUT_DIR/build-$config_name.log"

    # Copy binary
    mkdir -p "$build_dir"
    cp src/emacs "$build_dir/emacs"

    log_success "Built $config_name: $build_dir/emacs"
}

# Run benchmarks with a specific binary
run_benchmark() {
    local config_name="$1"
    local emacs_binary="$2"
    local output_file="$OUTPUT_DIR/${config_name}.csv"

    log_info "Running benchmarks for $config_name..."
    log_info "  Binary: $emacs_binary"
    log_info "  Output: $output_file"

    # Check if binary exists
    if [[ ! -x "$emacs_binary" ]]; then
        log_error "Binary not found or not executable: $emacs_binary"
        return 1
    fi

    # Determine benchmark function
    local bench_func="benchmark-run-all"
    if [[ "$QUICK_MODE" == "true" ]]; then
        bench_func="benchmark-quick"
        output_file="$OUTPUT_DIR/${config_name}-quick.csv"
    fi

    # Run Emacs with benchmark
    # Note: We need a display for rendering benchmarks
    if [[ -z "$DISPLAY" && -z "$WAYLAND_DISPLAY" ]]; then
        log_warn "No display available. Using Xvfb..."
        if command -v xvfb-run &> /dev/null; then
            xvfb-run -a "$emacs_binary" -Q \
                -l "$BENCHMARK_EL" \
                --eval "(${bench_func} \"${output_file}\")" \
                --eval "(kill-emacs 0)" \
                2>&1 | tee "$OUTPUT_DIR/run-$config_name.log"
        else
            log_error "No display and xvfb-run not available. Cannot run benchmarks."
            return 1
        fi
    else
        "$emacs_binary" -Q \
            -l "$BENCHMARK_EL" \
            --eval "(${bench_func} \"${output_file}\")" \
            --eval "(kill-emacs 0)" \
            2>&1 | tee "$OUTPUT_DIR/run-$config_name.log"
    fi

    if [[ -f "$output_file" ]]; then
        log_success "Benchmark complete: $output_file"
    else
        log_error "Benchmark failed - no output file generated"
        return 1
    fi
}

# Combine all CSV results
combine_results() {
    local combined="$OUTPUT_DIR/combined.csv"

    # Write header (from first file)
    local first_file=$(ls "$OUTPUT_DIR"/*.csv 2>/dev/null | grep -v combined | head -1)
    if [[ -n "$first_file" ]]; then
        head -1 "$first_file" > "$combined"

        # Append data from all files (skipping headers)
        for f in "$OUTPUT_DIR"/*.csv; do
            if [[ "$f" != "$combined" ]]; then
                tail -n +2 "$f" >> "$combined"
            fi
        done

        log_success "Combined results: $combined"
    fi
}

# Generate summary report
generate_summary() {
    local summary_file="$OUTPUT_DIR/summary.txt"

    {
        echo "Emacs Rendering Benchmark Summary"
        echo "=================================="
        echo "Run: $TIMESTAMP"
        echo ""

        # Parse CSV and compute averages
        if command -v awk &> /dev/null && [[ -f "$OUTPUT_DIR/combined.csv" ]]; then
            echo "Average times by backend and test (ms):"
            echo ""
            awk -F',' 'NR > 1 {
                key = $1 "," $2
                sum[key] += $4
                count[key]++
            }
            END {
                for (k in sum) {
                    printf "  %-30s %.2f ms\n", k, sum[k]/count[k]
                }
            }' "$OUTPUT_DIR/combined.csv" | sort
        fi

        echo ""
        echo "Files generated:"
        ls -la "$OUTPUT_DIR"/*.csv 2>/dev/null || echo "  No CSV files found"

    } > "$summary_file"

    cat "$summary_file"
    log_success "Summary saved to: $summary_file"
}

# Main execution
main() {
    echo ""
    echo "========================================"
    echo "Emacs Rendering Backend Benchmark"
    echo "========================================"
    echo ""

    save_system_info

    cd "$PROJECT_DIR"

    # Build and run Cairo
    if [[ "$RUN_CAIRO" == "true" ]]; then
        if [[ "$SKIP_BUILD" != "true" ]]; then
            build_emacs "cairo" "--with-pgtk --without-x"
        fi
        run_benchmark "cairo" "$PROJECT_DIR/build-cairo/emacs" || log_warn "Cairo benchmark failed"
    fi

    # Build and run Skia Raster (disable GL at runtime by using software rendering)
    if [[ "$RUN_SKIA_RASTER" == "true" ]]; then
        if [[ "$SKIP_BUILD" != "true" ]]; then
            # Build Skia without GL support
            # This requires configuring without GL libraries or using a no-GL Skia build
            build_emacs "skia-raster" "--with-pgtk --with-skia --without-x"
        fi
        # Force software rendering
        LIBGL_ALWAYS_SOFTWARE=1 \
        run_benchmark "skia-raster" "$PROJECT_DIR/build-skia-raster/emacs" || log_warn "Skia Raster benchmark failed"
    fi

    # Build and run Skia GL
    if [[ "$RUN_SKIA_GL" == "true" ]]; then
        if [[ "$SKIP_BUILD" != "true" ]]; then
            build_emacs "skia-gl" "--with-pgtk --with-skia --without-x"
        fi
        run_benchmark "skia-gl" "$PROJECT_DIR/build-skia-gl/emacs" || log_warn "Skia GL benchmark failed"
    fi

    # Combine and summarize
    combine_results
    generate_summary

    echo ""
    log_success "Benchmarking complete!"
    log_info "Results directory: $OUTPUT_DIR"
}

main "$@"
