# The filo command line behind each kept output: <example>.<what>.
case "$1" in
*.run) echo "run examples/${1%.run}.filo" ;;
*.dump) echo "dump examples/${1%.dump}.filo" ;;
*.trace) echo "run --trace examples/${1%.trace}.filo" ;;
esac
