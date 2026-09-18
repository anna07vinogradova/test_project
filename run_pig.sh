#!/usr/bin/env bash

delay=0.08

trap 'printf "\033[?25h"; clear; exit' INT TERM EXIT

# Скрываем курсор
printf '\033[?25l'

pig1='  ^-----^
 (  o o  )
  (  Y  )
   \(_)/
   /| |\
  / | | \'

pig2='  ^-----^
 (  o o  )
  (  Y  )
   \(_)/
    | |\
   /|  \'

while true; do
    cols=$(tput cols)

    for ((x=0; x<cols-12; x++)); do
        clear

        padding=$(printf "%${x}s" "")

        if (( x % 2 == 0 )); then
            while IFS= read -r line; do
                printf '%s%s\n' "$padding" "$line"
            done <<< "$pig1"
        else
            while IFS= read -r line; do
                printf '%s%s\n' "$padding" "$line"
            done <<< "$pig2"
        fi

        sleep "$delay"
    done
done
