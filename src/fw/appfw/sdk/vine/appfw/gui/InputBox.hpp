#pragma once

#include <vine/appfw/gui/Window.hpp>

#include <limits>
#include <optional>
#include <vector>

class QWidget;

V_APPFWGUI_NS_BEGIN

/**
 * @brief Modal dialog that collects several typed values in a single form.
 *
 * The dialog shows an optional prompt line, one editor per field and an
 * OK/Cancel button row. When OK is pressed the fields are validated; on
 * success the entered values are written back into the corresponding field
 * objects and the dialog is accepted, otherwise a warning is shown and the
 * dialog stays open.
 *
 * Field objects are borrowed (not owned) by the dialog, so they must outlive
 * the dialog. The synchronous static helper showAndWait() is the intended
 * entry point; a non-modal dialog can be created and shown via the Window
 * base API instead.
 *
 * Example:
 * @code
 * using namespace vine::appfw::gui;
 *
 * InputBox::StringField name(u8"名称", u8"", true, 1, 32);
 * InputBox::IntField    copies(u8"份数", 1, true, 1, 999);
 * InputBox::DoubleField scale(u8"比例", 1.0, true, 0.01, 100.0, 3);
 * InputBox::IndexField  align(u8"对齐", 0, true, { u8"左", u8"中", u8"右" });
 *
 * if (InputBox::showAndWait(parent, u8"新建对象", u8"请输入下列参数",
 *                           &name, &copies, &scale, &align)) {
 *     // ... use name.value / copies.value / scale.value / align.value
 * }
 * @endcode
 */
class V_APPFW_API InputBox : public Window {
    V_OBJECT_META_DECL;

  public:
    /// The kind of editor used for a field.
    enum class Kind
    {
        String, // Single-line text editor (QLineEdit).
        Int,    // Integer spin box (QSpinBox).
        Double, // Double spin box (QDoubleSpinBox).
        Index   // Drop-down list (QComboBox).
    };

    /// Common base of every concrete field definition.
    struct Field {
        Kind   kind;               // Editor kind; matches the concrete derived type.
        String label;              // Row label shown left of the editor.
        String validation_message; // Validation error message shown when the field is rejected.
        bool   required;           // Rejects an empty text or a missing selection.

      protected:
        Field(Kind kind, String label, bool required);
        virtual ~Field() = default;
    };

    /**
     * @brief Text field backed by a QLineEdit.
     */
    struct StringField : public Field {
        /**
         * @brief Constructs a string field.
         *
         * @param label     Row label shown left of the editor.
         * @param def_value Initial text of the editor.
         * @param required  When true an empty input is rejected.
         * @param min_len   Minimum accepted length for a non-empty input.
         * @param max_len   Maximum accepted length.
         */
        StringField(String label, String def_value = {}, bool required = true, int min_len = 1, int max_len = 100);
        String                def_value; // Initial text of the editor.
        int                   min_len;   // Minimum accepted length (non-empty input only).
        int                   max_len;   // Maximum accepted length.
        std::optional<String> value;     // Entered text; set when accepted.
    };

    /**
     * @brief Integer field backed by a QSpinBox.
     */
    struct IntField : public Field {
        /**
         * @brief Constructs an integer field.
         *
         * @param label     Row label shown left of the editor.
         * @param def_value Initial value of the editor.
         * @param required  Reserved; numeric editors always hold a value.
         * @param min_val   Lower range bound (inclusive).
         * @param max_val   Upper range bound (inclusive).
         * @param step      Arrow step; 0 uses one tenth of the range.
         */
        IntField(String label,
                 int    def_value = 0,
                 bool   required  = true,
                 int    min_val   = std::numeric_limits<int>::min(),
                 int    max_val   = std::numeric_limits<int>::max(),
                 int    step      = 0);
        int                def_value; // Initial value of the editor.
        int                min_val;   // Lower range bound (inclusive).
        int                max_val;   // Upper range bound (inclusive).
        int                step;      // Arrow step; 0 uses one tenth of the range.
        std::optional<int> value;     // Entered value; set when accepted.
    };

    /**
     * @brief Double field backed by a QDoubleSpinBox.
     */
    struct DoubleField : public Field {
        /**
         * @brief Constructs a double field.
         *
         * @param label     Row label shown left of the editor.
         * @param def_value Initial value of the editor.
         * @param required  Reserved; numeric editors always hold a value.
         * @param min_val   Lower range bound (inclusive).
         * @param max_val   Upper range bound (inclusive).
         * @param decimals  Number of decimals displayed.
         * @param step      Arrow step; 0 uses one tenth of the range.
         */
        DoubleField(String label,
                    double def_value = 0.0,
                    bool   required  = true,
                    double min_val   = -1e6,
                    double max_val   = 1e6,
                    int    decimals  = 2,
                    double step      = 0.0);
        double                def_value; // Initial value of the editor.
        double                min_val;   // Lower range bound (inclusive).
        double                max_val;   // Upper range bound (inclusive).
        int                   decimals;  // Number of decimals displayed.
        double                step;      // Arrow step; 0 uses one tenth of the range.
        std::optional<double> value;     // Entered value; set when accepted.
    };

    /**
     * @brief Index field backed by a QComboBox; the result is the selected
     * index, or -1 when nothing is selected.
     */
    struct IndexField : public Field {
        /**
         * @brief Constructs an index field.
         *
         * @param label     Row label shown left of the editor.
         * @param def_value Initial selection; a value of -1 selects nothing.
         * @param required  When true a missing selection is rejected.
         * @param items     Choices shown in the combo box.
         */
        IndexField(String label, int def_value = 0, bool required = true, std::vector<String> items = {});
        int                 def_value; // Initial selection; -1 selects nothing.
        std::vector<String> items;     // Choices shown in the combo box.
        std::optional<int>  value;     // Selected index; set when accepted.
    };

  public:
    /**
     * @brief Constructs an input dialog and builds the editors for the fields.
     *
     * The dialog does not take ownership of the field objects; they must stay
     * alive until the dialog is destroyed.
     *
     * @param parent  Parent widget; may be null for a top-level dialog.
     * @param title   Window title; an empty title falls back to a default.
     * @param prompt  Optional prompt text shown above the fields; may be empty.
     * @param fields  Field definitions to show, in row order.
     */
    InputBox(QWidget* parent, const String& title, const String& prompt, const std::vector<Field*>& fields);
    ~InputBox() override;

  public:
    /**
     * @brief Shows the dialog modally and reports whether it was accepted.
     *
     * When accepted the entered values are written back into the corresponding
     * field objects; when cancelled they are left untouched.
     *
     * @param parent  Parent widget; may be null for a top-level dialog.
     * @param title   Window title; an empty title falls back to a default.
     * @param prompt  Optional prompt text shown above the fields; may be empty.
     * @param fields  Field definitions to show, in row order.
     * @return True when accepted, false when cancelled.
     */
    static bool showAndWait(QWidget* parent, const String& title, const String& prompt, const std::vector<Field*>& fields);

    /**
     * @brief Overload of showAndWait() that takes the fields individually.
     *
     * @param parent  Parent widget; may be null for a top-level dialog.
     * @param title   Window title; an empty title falls back to a default.
     * @param prompt  Optional prompt text shown above the fields; may be empty.
     * @param fields  Pointers to the field objects to show, in row order.
     * @return True when accepted, false when cancelled.
     */
    template <typename... TFields>
    static bool showAndWait(QWidget* parent, const String& title, const String& prompt, TFields*... fields)
    {
        return showAndWait(parent, title, prompt, std::vector<Field*>{ fields... });
    }

  private:
    void buildUi();
    void confirm();
    void cancel();

    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

V_APPFWGUI_NS_END
