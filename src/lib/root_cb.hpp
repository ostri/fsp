// processing_callback.hpp
#pragma once

#include <memory>
#include <optional>
#include <string>
#include "error_info.hpp"
#include "segment_result.hpp"
#include "xml_segment.hpp"
#include "xml_processor.hpp"

namespace fsp
{

  enum class callback_action : std::uint8_t
  {
    continue_processing,
    abort_processing
  };

  class root_cb
  {
  public:
    explicit root_cb(const fsp_logger& log)
    : log_(log)
    {
    }
    virtual ~root_cb()                 = default; // destructor
    root_cb(const root_cb& o)          = default; // copy constructor
    root_cb(root_cb&&)                 = delete;
    root_cb& operator=(const root_cb&) = delete;
    root_cb& operator=(root_cb&&)      = delete;
    /**
     * @brief instance clonning
     * The method must be overriden in descendant class and it is used to prepare worker private data
     * @return std::unique_ptr<root_cb>
     */
    [[nodiscard]] virtual std::unique_ptr<root_cb> clone() const = 0;
    /// ---- methods are executed in the same clonned context (one for each thread) ---
    /**
     * @brief executed before first document started
     *
     * @param worker_id thread id of the worker
     */
    virtual void worker_start([[maybe_unused]] int worker_id) { }
    /**
     * @brief executed before new document processing starts
     *
     * @param worker_id thread id
     * @param xml_path  xml path of the document to be processed (can be empty if from buffer)
     * @param xsd_path  xml path to the xsd grammar (can be empty if no grammar)
     */
    virtual void doc_start([[maybe_unused]] int                worker_id,
                           [[maybe_unused]] const std::string& xml_path,
                           [[maybe_unused]] const std::string& xsd_path)
    {
    }
    /**
     * @brief executed after last segmetn from the file was processed
     *
     * @param worker_id thread id
     * @param stats file processing statistics
     */
    virtual void doc_end( //
      [[maybe_unused]] int                         worker_id,
      [[maybe_unused]] const xml_processor::stats& stats)
    {
    }
    /**
     * @brief executed after last document was closed
     *
     * @param worker_id thread id
     */
    virtual void worker_end([[maybe_unused]] int worker_id) { }
    /**
     * @brief executed for each segment to be processed
     *
     * @param worker_id thread id
     * @param seg  segment information
     * @param result result information
     * @return callback_action
     */
    virtual callback_action process_seg([[maybe_unused]] int                   worker_id,
                                        [[maybe_unused]] const xml_segment&    seg,
                                        [[maybe_unused]] const segment_result& result)
    { return callback_action::continue_processing; }
    /**
     * @brief executed at the end of validation
     *
     * @param error optional variable with error description. Absent if everything is ok
     * @param xml_path xml path to the file that was validated (can be null if buffer)
     * @param xsd_path xsd path to the file that holds the grammar file
     *
     * The method is executed upon finish of the validation. If finished well no error object is present.
     * Otherwise the error parameter holds the description of the error.
     */
    virtual void validation_finished([[maybe_unused]] const std::optional<error_info>& error,
                                     [[maybe_unused]] const std::string&               xml_path,
                                     [[maybe_unused]] const std::string&               xsd_path)
    {
    }
    /**
     * @brief reference to logger
     *
     * @return const fsp_logger&
     */
    [[nodiscard]] const fsp_logger& log() const { return log_; }
  private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    const fsp_logger& log_; // logger reference
  };

} // namespace fsp