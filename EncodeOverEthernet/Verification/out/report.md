# Hardware-in-the-loop results

Each image encoded on the PYNQ-Z2 and byte-compared against CharLS; per-image results are in results.csv.

OpenJLS v1.6.0 (version register), FCLK0 100 MHz.

| Depth | OpenJLS commit | Sources hash | Build hash |
|--:|--|--|--|
| 8 | 50892d8dc973 | ec242ecd5e05 | 9335daea3cce |
| 9 | 50892d8dc973 | ec242ecd5e05 | 9335daea3cce |
| 10 | 50892d8dc973 | ec242ecd5e05 | 9335daea3cce |
| 11 | 50892d8dc973 | ec242ecd5e05 | 9335daea3cce |
| 12 | 50892d8dc973 | ec242ecd5e05 | 9335daea3cce |
| 13 | 50892d8dc973 | ec242ecd5e05 | 9335daea3cce |
| 14 | 50892d8dc973 | ec242ecd5e05 | 9335daea3cce |
| 15 | 50892d8dc973 | ec242ecd5e05 | 9335daea3cce |
| 16 | 50892d8dc973 | ec242ecd5e05 | 9335daea3cce |

## One image per reset

| Depth | Images | Pass | Skip | Fail | Input (B) | JLS checked (B) | Ratio |
|--:|--:|--:|--:|--:|--:|--:|--:|
| 8 | 735 | 735 | 0 | 0 | 289122749 | 143542511 | 2.01:1 |
| 9 | 604 | 604 | 0 | 0 | 74079498 | 18943216 | 3.91:1 |
| 10 | 605 | 605 | 0 | 0 | 82050290 | 25710786 | 3.19:1 |
| 11 | 604 | 604 | 0 | 0 | 72086778 | 25018985 | 2.88:1 |
| 12 | 604 | 604 | 0 | 0 | 76127112 | 30560324 | 2.49:1 |
| 13 | 604 | 604 | 0 | 0 | 75059158 | 33142477 | 2.26:1 |
| 14 | 605 | 605 | 0 | 0 | 76592816 | 37918533 | 2.02:1 |
| 15 | 604 | 604 | 0 | 0 | 76214694 | 39585476 | 1.93:1 |
| 16 | 694 | 694 | 0 | 0 | 423372708 | 287179938 | 1.47:1 |
| **Total** | 5659 | 5659 | 0 | 0 | 1244705803 | 641602246 | 1.94:1 |

## No-reset streams

Same-size images back to back, one core reset per set. Each set's output is throttled in the PL to its ready level, the r<N> in its name.

| Depth | Images | Pass | Skip | Fail | Input (B) | JLS checked (B) | Ratio |
|--:|--:|--:|--:|--:|--:|--:|--:|
| 8 | 1200 | 1200 | 0 | 0 | 19660800 | 9195876 | 2.14:1 |
| 9 | 600 | 600 | 0 | 0 | 19660800 | 5387561 | 3.65:1 |
| 10 | 600 | 600 | 0 | 0 | 19660800 | 6259135 | 3.14:1 |
| 11 | 600 | 600 | 0 | 0 | 19660800 | 7605529 | 2.59:1 |
| 12 | 600 | 600 | 0 | 0 | 19660800 | 8462558 | 2.32:1 |
| 13 | 600 | 600 | 0 | 0 | 19660800 | 9312562 | 2.11:1 |
| 14 | 600 | 600 | 0 | 0 | 19660800 | 10905679 | 1.80:1 |
| 15 | 600 | 600 | 0 | 0 | 19660800 | 11719599 | 1.68:1 |
| 16 | 600 | 600 | 0 | 0 | 19660800 | 13136192 | 1.50:1 |
| **Total** | 6000 | 6000 | 0 | 0 | 176947200 | 81984691 | 2.16:1 |
