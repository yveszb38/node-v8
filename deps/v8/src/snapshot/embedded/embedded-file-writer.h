// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_SNAPSHOT_EMBEDDED_EMBEDDED_FILE_WRITER_H_
#define V8_SNAPSHOT_EMBEDDED_EMBEDDED_FILE_WRITER_H_

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>

#include "src/common/globals.h"
#include "src/snapshot/embedded/embedded-data.h"
#include "src/snapshot/embedded/platform-embedded-file-writer-base.h"

#if defined(V8_OS_WIN64)
#include "src/diagnostics/unwinding-info-win64.h"
#endif  // V8_OS_WIN64

namespace v8 {
namespace internal {

static constexpr char kDefaultEmbeddedVariant[] = "Default";

struct LabelInfo {
  int offset;
  std::string name;
};

// Detailed source-code information about builtins can only be obtained by
// registration on the isolate during compilation.
class EmbeddedFileWriterInterface {
 public:
  // We maintain a database of filenames to synthetic IDs.
  virtual int LookupOrAddExternallyCompiledFilename(const char* filename) = 0;
  virtual const char* GetExternallyCompiledFilename(int index) const = 0;
  virtual int GetExternallyCompiledFilenameCount() const = 0;

  // The isolate will call the method below just prior to replacing the
  // compiled builtin Code objects with trampolines.
  virtual void PrepareBuiltinSourcePositionMap(Builtins* builtins) = 0;

  virtual void PrepareBuiltinLabelInfoMap(int create_offset, int invoke_offset,
                                          int arguments_adaptor_offset) = 0;

#if defined(V8_OS_WIN64)
  virtual void SetBuiltinUnwindData(
      int builtin_index,
      const win64_unwindinfo::BuiltinUnwindInfo& unwinding_info) = 0;
#endif  // V8_OS_WIN64
};

// Generates the embedded.S file which is later compiled into the final v8
// binary. Its contents are exported through two symbols:
//
// v8_<variant>_embedded_blob_ (intptr_t):
//     a pointer to the start of the embedded blob.
// v8_<variant>_embedded_blob_size_ (uint32_t):
//     size of the embedded blob in bytes.
//
// The variant is usually "Default" but can be modified in multisnapshot builds.
class EmbeddedFileWriter : public EmbeddedFileWriterInterface {
 public:
  int LookupOrAddExternallyCompiledFilename(const char* filename) override;
  const char* GetExternallyCompiledFilename(int fileid) const override;
  int GetExternallyCompiledFilenameCount() const override;

  void PrepareBuiltinSourcePositionMap(Builtins* builtins) override;

  void PrepareBuiltinLabelInfoMap(int create_offset, int invoke_create,
                                  int arguments_adaptor_offset) override;

#if defined(V8_OS_WIN64)
  void SetBuiltinUnwindData(
      int builtin_index,
      const win64_unwindinfo::BuiltinUnwindInfo& unwinding_info) override {
    DCHECK_LT(builtin_index, Builtins::builtin_count);
    unwind_infos_[builtin_index] = unwinding_info;
  }
#endif  // V8_OS_WIN64

  void SetEmbeddedFile(const char* embedded_src_path) {
    embedded_src_path_ = embedded_src_path;
  }

  void SetEmbeddedVariant(const char* embedded_variant) {
    if (embedded_variant == nullptr) return;
    embedded_variant_ = embedded_variant;
  }

  void SetTargetArch(const char* target_arch) { target_arch_ = target_arch; }

  void SetTargetOs(const char* target_os) { target_os_ = target_os; }

  void WriteEmbedded(const i::EmbeddedData* blob) const {
    MaybeWriteEmbeddedFile(blob);
  }

 private:
  void MaybeWriteEmbeddedFile(const i::EmbeddedData* blob) const {
    if (embedded_src_path_ == nullptr) return;

    FILE* fp = GetFileDescriptorOrDie(embedded_src_path_);

    std::unique_ptr<PlatformEmbeddedFileWriterBase> writer =
        NewPlatformEmbeddedFileWriter(target_arch_, target_os_);
    writer->SetFile(fp);

    WriteFilePrologue(writer.get());
    WriteExternalFilenames(writer.get());
    WriteMetadataSection(writer.get(), blob);
    WriteInstructionStreams(writer.get(), blob);
    WriteFileEpilogue(writer.get(), blob);

    fclose(fp);
  }

  static FILE* GetFileDescriptorOrDie(const char* filename) {
    FILE* fp = v8::base::OS::FOpen(filename, "wb");
    if (fp == nullptr) {
      i::PrintF("Unable to open file \"%s\" for writing.\n", filename);
      exit(1);
    }
    return fp;
  }

  void WriteFilePrologue(PlatformEmbeddedFileWriterBase* w) const {
    w->Comment("Autogenerated file. Do not edit.");
    w->Newline();
    w->FilePrologue();
  }

  void WriteExternalFilenames(PlatformEmbeddedFileWriterBase* w) const {
#ifndef DEBUG
    // Release builds must not contain debug infos.
    CHECK_EQ(external_filenames_by_index_.size(), 0);
#endif

    w->Comment(
        "Source positions in the embedded blob refer to filenames by id.");
    w->Comment("Assembly directives here map the id to a filename.");
    w->Newline();

    // Write external filenames.
    int size = static_cast<int>(external_filenames_by_index_.size());
    for (int i = 0; i < size; i++) {
      w->DeclareExternalFilename(ExternalFilenameIndexToId(i),
                                 external_filenames_by_index_[i]);
    }
  }

  // Fairly arbitrary but should fit all symbol names.
  static constexpr int kTemporaryStringLength = 256;

  std::string EmbeddedBlobCodeDataSymbol() const {
    i::EmbeddedVector<char, kTemporaryStringLength>
        embedded_blob_code_data_symbol;
    i::SNPrintF(embedded_blob_code_data_symbol,
                "v8_%s_embedded_blob_code_data_", embedded_variant_);
    return std::string{embedded_blob_code_data_symbol.begin()};
  }

  std::string EmbeddedBlobMetadataDataSymbol() const {
    i::EmbeddedVector<char, kTemporaryStringLength>
        embedded_blob_metadata_data_symbol;
    i::SNPrintF(embedded_blob_metadata_data_symbol,
                "v8_%s_embedded_blob_metadata_data_", embedded_variant_);
    return std::string{embedded_blob_metadata_data_symbol.begin()};
  }

  void WriteMetadataSection(PlatformEmbeddedFileWriterBase* w,
                            const i::EmbeddedData* blob) const {
    w->Comment("The embedded blob metadata starts here.");
    w->SectionRoData();
    w->AlignToDataAlignment();
    w->DeclareLabel(EmbeddedBlobMetadataDataSymbol().c_str());

    WriteBinaryContentsAsInlineAssembly(w, blob->metadata(),
                                        blob->metadata_size());
  }

  void WriteBuiltin(PlatformEmbeddedFileWriterBase* w,
                    const i::EmbeddedData* blob, const int builtin_id) const;

  void WriteBuiltinLabels(PlatformEmbeddedFileWriterBase* w,
                          std::string name) const;

  void WriteInstructionStreams(PlatformEmbeddedFileWriterBase* w,
                               const i::EmbeddedData* blob) const {
    w->Comment("The embedded blob data starts here. It contains the builtin");
    w->Comment("instruction streams.");
    w->SectionText();
    w->AlignToCodeAlignment();
    w->DeclareLabel(EmbeddedBlobCodeDataSymbol().c_str());

    for (int i = 0; i < i::Builtins::builtin_count; i++) {
      if (!blob->ContainsBuiltin(i)) continue;

      WriteBuiltin(w, blob, i);
    }
    w->Newline();
  }

  void WriteFileEpilogue(PlatformEmbeddedFileWriterBase* w,
                         const i::EmbeddedData* blob) const;

#if defined(V8_OS_WIN_X64)
  void WriteUnwindInfoEntry(PlatformEmbeddedFileWriterBase* w,
                            uint64_t rva_start, uint64_t rva_end) const;
#endif

  static void WriteBinaryContentsAsInlineAssembly(
      PlatformEmbeddedFileWriterBase* w, const uint8_t* data, uint32_t size);

  // In assembly directives, filename ids need to begin with 1.
  static constexpr int kFirstExternalFilenameId = 1;
  static int ExternalFilenameIndexToId(int index) {
    return kFirstExternalFilenameId + index;
  }
  static int ExternalFilenameIdToIndex(int id) {
    return id - kFirstExternalFilenameId;
  }

 private:
  std::vector<byte> source_positions_[Builtins::builtin_count];
  std::vector<LabelInfo> label_info_[Builtins::builtin_count];

#if defined(V8_OS_WIN64)
  win64_unwindinfo::BuiltinUnwindInfo unwind_infos_[Builtins::builtin_count];
#endif  // V8_OS_WIN64

  std::map<const char*, int> external_filenames_;
  std::vector<const char*> external_filenames_by_index_;

  // The file to generate or nullptr.
  const char* embedded_src_path_ = nullptr;

  // The variant is only used in multi-snapshot builds and otherwise set to
  // "Default".
  const char* embedded_variant_ = kDefaultEmbeddedVariant;

  // {target_arch} and {target_os} control the generated assembly format. Note
  // these may differ from both host- and target-platforms specified through
  // e.g. V8_OS_* and V8_TARGET_ARCH_* defines.
  const char* target_arch_ = nullptr;
  const char* target_os_ = nullptr;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_SNAPSHOT_EMBEDDED_EMBEDDED_FILE_WRITER_H_
