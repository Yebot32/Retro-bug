// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// DEX fixture naming three of tests/corpus/jni_bridge_fixture.c's Java_rb_Fixture_* natives (the repackager's
// generated-stub row): the library's other Java_* exports have no declaration here and must be left out.
package rb;

class Fixture {
    static native long mix(int a, long b, float c, double d, int e, long f, float g, double h, int i, int j, int k,
                           int l, float m, float n, double o, double p, float q, double r);
    static native double half(float x);
    native Object echoObject(Object o);
}
