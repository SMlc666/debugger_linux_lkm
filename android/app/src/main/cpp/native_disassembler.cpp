#include <jni.h>

#include <android/log.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

#include "capstone/capstone.h"

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
