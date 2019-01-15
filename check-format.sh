
set -u

unformatted_files=""
for f in `find . -name *.h -o -name *.hh -o -name *.cc`; do
  d=`clang-format -style=file $f | diff $f -`
  if [ "$d" != "" ]; then
    if [ "$unformatted_files" != "" ]; then
      unformatted_files+=$'\n'
    fi
    unformatted_files+="$f"
  fi
done

if [ "$unformatted_files" != "" ]; then
  echo "$unformatted_files"
  exit 1
fi
