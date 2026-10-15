// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
package com.retrobug.launcher;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;

import com.android.apksig.ApkSigner;

import java.io.File;
import java.math.BigInteger;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.cert.Certificate;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

import javax.security.auth.x500.X500Principal;

/**
 * Signs a repackaged APK with a key that lives in this phone's AndroidKeyStore and never leaves it.
 *
 * One key for every game, created on first use and reused after: a second repack of the same game must be
 * installable as an update of the first, and the platform only allows that when the two are signed by the
 * same certificate. A key per run would turn every repack into "uninstall first, losing the save".
 *
 * apksig (the library behind `apksigner`, shipped inside this APK's dex) does the signing with v1, v2 and
 * v3 for minSdk 24 (the floor ApkRepackager raises minSdkVersion to); it also aligns STORED entries
 * while it writes, so the unsigned input need not be zip-aligned.
 */
final class RepackSigner {
    private RepackSigner() {}

    static final String ALIAS = "retrobug-repack";
    private static final String KEYSTORE = "AndroidKeyStore";

    /**
     * Synchronized on the class: key lookup-or-create and signing run as one unit, so two concurrent repacks
     * can never both see the alias missing and generate it twice (the second key would make the first
     * repack's output un-updatable by every later one).
     */
    static synchronized void sign(File unsigned, File signed) throws Exception {
        KeyStore ks = KeyStore.getInstance(KEYSTORE);
        ks.load(null);
        if (!ks.containsAlias(ALIAS)) create();
        PrivateKey key = (PrivateKey) ks.getKey(ALIAS, null);
        Certificate cert = ks.getCertificate(ALIAS);
        if (key == null || !(cert instanceof X509Certificate))
            throw new IllegalStateException("AndroidKeyStore alias " + ALIAS + " has no usable key/certificate");
        List<X509Certificate> chain = new ArrayList<>(Collections.singletonList((X509Certificate) cert));

        ApkSigner.SignerConfig signer = new ApkSigner.SignerConfig.Builder("RETROBUG", key, chain).build();
        if (signed.exists() && !signed.delete()) throw new java.io.IOException("could not replace " + signed);
        new ApkSigner.Builder(Collections.singletonList(signer))
                .setInputApk(unsigned)
                .setOutputApk(signed)
                .setV1SigningEnabled(true)
                .setV2SigningEnabled(true)
                .setV3SigningEnabled(true)
                .setMinSdkVersion(ApkRepackager.MIN_SDK_FLOOR)   // the output's manifest declares at least this
                .setCreatedBy("RetroBug")
                .build()
                .sign();
    }

    private static void create() throws Exception {
        KeyPairGenerator gen = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, KEYSTORE);
        gen.initialize(new KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_SIGN)
                .setKeySize(2048)
                .setDigests(KeyProperties.DIGEST_SHA256, KeyProperties.DIGEST_SHA512)
                .setSignaturePaddings(KeyProperties.SIGNATURE_PADDING_RSA_PKCS1)
                .setCertificateSubject(new X500Principal("CN=RetroBug repack, O=RetroBug"))
                .setCertificateSerialNumber(BigInteger.ONE)
                .build());
        gen.generateKeyPair();
    }
}
