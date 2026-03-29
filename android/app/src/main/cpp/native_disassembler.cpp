#include <jni.h>

#include <android/log.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

#include "capstone/capstone.h"
#include "keystone/keystone.h"

namespace {

static const char *k_log_tag = "lkmdbg-native";

static std::string format_bytes(const cs_insn &insn)
{
	std::string text;
	char chunk[4];

	for (size_t i = 0; i < insn.size; ++i) {
		snprintf(chunk, sizeof(chunk), "%02x", insn.bytes[i]);
		if (!text.empty())
			text.push_back(' ');
		text.append(chunk);
	}

	return text;
}

static jobjectArray make_string_array(JNIEnv *env, const std::vector<std::string> &lines)
{
	jclass string_class;
	jobjectArray out;

	string_class = env->FindClass("java/lang/String");
	if (!string_class)
		return nullptr;

	out = env->NewObjectArray(static_cast<jsize>(lines.size()), string_class, nullptr);
	if (!out)
		return nullptr;

	for (jsize i = 0; i < static_cast<jsize>(lines.size()); ++i) {
		jstring text = env->NewStringUTF(lines[i].c_str());

		if (!text)
			return out;
		env->SetObjectArrayElement(out, i, text);
		env->DeleteLocalRef(text);
	}

	return out;
}

static void throw_java_exception(JNIEnv *env, const char *class_name,
				 const std::string &message)
{
	jclass ex_class;

	if (!env || message.empty())
		return;
	ex_class = env->FindClass(class_name);
	if (!ex_class)
		return;
	env->ThrowNew(ex_class, message.c_str());
}

} // namespace

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_smlc666_lkmdbg_data_NativeDisassembler_nativeDisassembleArm64(
	JNIEnv *env, jclass, jlong base_address, jbyteArray code_bytes,
	jint max_instructions)
{
	std::vector<uint8_t> code;
	std::vector<std::string> lines;
	cs_insn *insn = nullptr;
	csh handle = 0;
	jsize code_size;
	size_t count;
	const uint64_t address = static_cast<uint64_t>(base_address);

	if (!code_bytes)
		return make_string_array(env, lines);

	code_size = env->GetArrayLength(code_bytes);
	if (code_size <= 0)
		return make_string_array(env, lines);

	code.resize(static_cast<size_t>(code_size));
	env->GetByteArrayRegion(code_bytes, 0, code_size,
			       reinterpret_cast<jbyte *>(code.data()));

	if (cs_open(CS_ARCH_AARCH64, CS_MODE_LITTLE_ENDIAN, &handle) != CS_ERR_OK) {
		__android_log_print(ANDROID_LOG_ERROR, k_log_tag,
				    "cs_open failed for arm64 preview disassembly");
		return make_string_array(env, lines);
	}

	cs_option(handle, CS_OPT_DETAIL, CS_OPT_OFF);
	count = cs_disasm(handle, code.data(), code.size(), address,
			  max_instructions > 0 ? static_cast<size_t>(max_instructions) : 24U,
			  &insn);
	for (size_t i = 0; i < count; ++i) {
		const std::string bytes = format_bytes(insn[i]);
		char buffer[256];

		snprintf(buffer, sizeof(buffer), "0x%016" PRIx64 "  %-23s  %s%s%s",
			 static_cast<uint64_t>(insn[i].address), bytes.c_str(),
			 insn[i].mnemonic,
			 insn[i].op_str[0] ? " " : "",
			 insn[i].op_str);
		lines.emplace_back(buffer);
	}

	if (!count)
		lines.emplace_back("No AArch64 instruction decoded");

	cs_free(insn, count);
	cs_close(&handle);
	return make_string_array(env, lines);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_smlc666_lkmdbg_data_NativeAssembler_nativeAssembleArm64(
	JNIEnv *env, jclass, jlong base_address, jstring source_text)
{
	ks_engine *engine = nullptr;
	jbyteArray out = nullptr;
	unsigned char *encoded = nullptr;
	size_t encoded_size = 0;
	size_t statement_count = 0;
	const char *source = nullptr;
	ks_err err;
	ks_err asm_err;
	int asm_status;
	std::string error;

	if (!source_text) {
		throw_java_exception(env, "java/lang/IllegalArgumentException",
				     "assembly source must not be null");
		return nullptr;
	}

	source = env->GetStringUTFChars(source_text, nullptr);
	if (!source)
		return nullptr;
	if (!source[0]) {
		env->ReleaseStringUTFChars(source_text, source);
		throw_java_exception(env, "java/lang/IllegalArgumentException",
				     "assembly source must not be empty");
		return nullptr;
	}

	err = ks_open(KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN, &engine);
	if (err != KS_ERR_OK) {
		env->ReleaseStringUTFChars(source_text, source);
		error = std::string("ks_open failed: ") + ks_strerror(err);
		throw_java_exception(env, "java/lang/IllegalStateException", error);
		return nullptr;
	}

	asm_status = ks_asm(engine, source, static_cast<uint64_t>(base_address),
			    &encoded, &encoded_size, &statement_count);
	env->ReleaseStringUTFChars(source_text, source);
	if (asm_status != 0) {
		asm_err = ks_errno(engine);
		error = std::string("arm64 assembly failed: ") + ks_strerror(asm_err);
		if (asm_err == KS_ERR_ASM_INVALIDOPERAND)
			error.append(" (invalid operand)");
		ks_close(engine);
		throw_java_exception(env, "java/lang/IllegalArgumentException", error);
		return nullptr;
	}

	out = env->NewByteArray(static_cast<jsize>(encoded_size));
	if (out && encoded_size) {
		env->SetByteArrayRegion(out, 0, static_cast<jsize>(encoded_size),
				       reinterpret_cast<const jbyte *>(encoded));
	}

	ks_free(encoded);
	ks_close(engine);
	return out;
}
