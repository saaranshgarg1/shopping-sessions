# Decode files from parquet_probe.cpp using an isolated environment with pyarrow.
from datetime import date, timedelta
import sys

import pyarrow as pa
import pyarrow.parquet as pq

d = sys.argv[1] if len(sys.argv) > 1 else "."

# nonempty.parquet: top_category nullable string, day DATE, sessions int64
t = pq.read_table(f"{d}/nonempty.parquet")
schema = t.schema
assert schema.names == ["top_category", "day", "sessions"], schema.names
f = schema.field("top_category")
assert f.type.equals(pa.string()), f.type
assert f.nullable, "top_category must be nullable"
assert schema.field("day").type.equals(pa.date32()), schema.field("day").type
assert not schema.field("day").nullable
assert schema.field("sessions").type.equals(pa.int64()), schema.field("sessions").type
assert t.num_rows == 3000, t.num_rows
col = t.column("top_category").to_pylist()
days = t.column("day").to_pylist()
sess = t.column("sessions").to_pylist()
for i in range(3000):
    if i % 7 == 0:
        assert col[i] is None, (i, col[i])
    else:
        assert col[i] == f"cat_{i % 23}", (i, col[i])
    assert days[i] == date(1970, 1, 1) + timedelta(days=19000 + i)
    assert sess[i] == i * 3, (i, sess[i])
print("nonempty.parquet: schema+values OK (3000 rows, nulls, date32, int64)")

# empty.parquet: 0 rows, correct schema
t0 = pq.read_table(f"{d}/empty.parquet")
assert t0.num_rows == 0, t0.num_rows
assert t0.schema.names == ["user_id"], t0.schema.names
assert t0.schema.field("user_id").type.equals(pa.string())
print("empty.parquet: 0-row schema-only file reads OK")

# required.parquet: required columns, no nulls
t2 = pq.read_table(f"{d}/required.parquet")
assert t2.schema.names == ["user_id", "revenue_cents"], t2.schema.names
assert not t2.schema.field("user_id").nullable
assert not t2.schema.field("revenue_cents").nullable
assert t2.column("user_id").to_pylist() == ["u1", "u2"]
assert t2.column("revenue_cents").to_pylist() == [0, 12345]
print("required.parquet: values OK")

views = pq.read_table(f"{d}/views.parquet")
assert views.schema.names == ["user_id", "top_category"]
assert not views.schema.field("user_id").nullable
assert views.schema.field("top_category").nullable
assert views.column("user_id").to_pylist() == ["u_" + "a" * 40, "unicode_é", ""]
assert views.column("top_category").to_pylist() == ["unicode_é", None, ""]
print("views.parquet: borrowed long/UTF-8/empty strings and nulls OK")

for count in range(18):
    for mode in range(3):
        t = pq.read_table(f"{d}/null_{count}_{mode}.parquet")
        assert t.schema.field("top_category").nullable
        assert t.column("top_category").to_pylist() == [
            ("" if i % 2 else "unicode_é") if mode == 0 or (mode == 2 and i % 3 != 0) else None
            for i in range(count)
        ]

t = pq.read_table(f"{d}/multipage.parquet")
assert t.num_rows == 5001
values = ["x" * 1000 + str(i) for i in range(5001)]
assert t.column("value").to_pylist() == values
assert t.column("top_category").to_pylist() == [v if i % 7 else None for i, v in enumerate(values)]
assert pq.read_metadata(f"{d}/multipage.parquet").num_row_groups == 1
print("ALL PARQUET CHECKS PASSED (null padding, all-null, empty, multipage)")
