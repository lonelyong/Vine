# test_data/robots

## irb_1600_10_145.vdev

ABB IRB 1600-10/1.45 six-axis manipulator, 8 links, `length_unit="mm"`.

It is a **device package** (a ZIP holding `device.xml` plus the geometry), loaded with
`DeviceIO::loadPkg()` - not with `loadXml()`, which reads a bare XML file.

Each mesh is **one binary STL file** (`geoms/meshN.stl`), and the description points at it with
`geometry=`. 14 mesh elements reference 13 files (`mesh12` is shared by a visual and a collision),
98 674 triangles in total.

### 2026-09-29: converted from the split-entry form

It used to hold three raw `.bin` entries per mesh (`meshN.positions.bin`, `.normals.bin`,
`.indices.bin`, host byte order, no header) with the four attribute names in the description.
Those entries are gone; nothing in the tree reads that form any more.

The conversion kept the geometry exactly: every triangle corner is the same float32 in the same
order, each mesh keeps the triangle count it had, and the facet normals are the mesh's own values
(the source's normals were already one per facet - 98 448 of the 98 674 triangles carry the same
value in every corner, and the remaining 226 agree to within 1e-4, where the corners' own value is
written). Everything outside the mesh elements of `device.xml` is byte-for-byte what it was.
