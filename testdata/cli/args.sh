# The filo command line behind each kept output: <example>.<what>.
case "$1" in
demo.run) echo "run build/cli/demo.fbb double" ;;
demo.dump) echo "dump build/cli/demo.fbb" ;;
*.run) echo "run examples/${1%.run}.filo" ;;
*.dump) echo "dump examples/${1%.dump}.filo" ;;
*.trace) echo "run --trace examples/${1%.trace}.filo" ;;
*.tree) echo "show tree examples/${1%.tree}.filo" ;;
*.folded) echo "show folded examples/${1%.folded}.filo" ;;
*.ir) echo "show ir examples/${1%.ir}.filo" ;;
*.both) echo "run --both examples/${1%.both}.filo" ;;
esac
