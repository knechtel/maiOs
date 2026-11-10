# SQLite for maiOs

`sqlite3.c`, `sqlite3.h`, and `sqlite3ext.h` are the SQLite 3.54.1 amalgamation
from <https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip>.
The downloaded archive had SHA3-256
`92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`.
SQLite is in the public domain; see the notice at the top of `sqlite3.c`.

`sqlite3_maios.c` builds the amalgamation for the freestanding maiOs target.
`maios_vfs.c` supplies SQLite's VFS over the ext4 driver.
