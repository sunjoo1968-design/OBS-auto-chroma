#include "dock.hpp"
#include "ac-types.hpp"
#include "analyzer.hpp"
#include "filter.hpp"
#include "preview-driver.hpp"
#include "preview-widget.hpp"
#include "scene-rotation.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QSpinBox>
#include <QStyle>
#include <QDoubleSpinBox>
#include <QTimer>
#include <QThread>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <memory>

namespace ac {
namespace {
constexpr const char *DockId = "sunjoo-auto-chroma-dock";
QString utf8(const char *s) { return QString::fromUtf8(s ? s : ""); }
bool isLive() { return obs_frontend_streaming_active() || obs_frontend_recording_active(); }
QPushButton *resetButton(QWidget *parent)
{
    auto *button = new QPushButton(parent);
    button->setIcon(button->style()->standardIcon(QStyle::SP_BrowserReload));
    button->setIconSize(QSize(18, 18)); button->setFixedSize(32, 28);
    button->setToolTip(QStringLiteral("기본값 복원")); button->setAccessibleName(QStringLiteral("기본값 복원"));
    return button;
}
QWidget *sliderControl(QSpinBox *spin)
{
    auto *body = new QWidget;
    auto *row = new QHBoxLayout(body); row->setContentsMargins(0, 0, 0, 0);
    auto *slider = new QSlider(Qt::Horizontal, body); slider->setMinimumWidth(120); spin->setMaximumWidth(90); slider->setRange(spin->minimum(), spin->maximum()); slider->setValue(spin->value());
    auto *reset = resetButton(body);
    const int initial = spin->value();
    QObject::connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
    QObject::connect(spin, &QSpinBox::valueChanged, slider, &QSlider::setValue);
    QObject::connect(reset, &QPushButton::clicked, spin, [spin, initial] { spin->setValue(initial); });
    row->addWidget(slider, 1); row->addWidget(spin); row->addWidget(reset); return body;
}
QWidget *sliderControl(QDoubleSpinBox *spin)
{
    auto *body = new QWidget;
    auto *row = new QHBoxLayout(body); row->setContentsMargins(0, 0, 0, 0);
    auto *slider = new QSlider(Qt::Horizontal, body); slider->setMinimumWidth(120); spin->setMaximumWidth(90); constexpr double scale = 1000;
    slider->setRange(int(std::round(spin->minimum() * scale)), int(std::round(spin->maximum() * scale))); slider->setValue(int(std::round(spin->value() * scale)));
    auto *reset = resetButton(body);
    const double initial = spin->value();
    QObject::connect(slider, &QSlider::valueChanged, spin, [spin](int value) { spin->setValue(value / 1000.); });
    QObject::connect(spin, &QDoubleSpinBox::valueChanged, slider, [slider](double value) { slider->setValue(int(std::round(value * 1000))); });
    QObject::connect(reset, &QPushButton::clicked, spin, [spin, initial] { spin->setValue(initial); });
    row->addWidget(slider, 1); row->addWidget(spin); row->addWidget(reset); return body;
}
QJsonObject settingsJson(obs_source_t *source)
{
    obs_data_t *settings = obs_source_get_settings(source);
    const auto obj = QJsonDocument::fromJson(QByteArray(obs_data_get_json(settings))).object();
    obs_data_release(settings); return obj;
}
obs_data_t *fromJson(const QJsonObject &obj)
{
    const auto json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    return obs_data_create_from_json(json.constData());
}
bool isKeyer(obs_source_t *source)
{
    const auto id = utf8(obs_source_get_unversioned_id(source));
    return id == "chroma_key_filter" || id == "color_key_filter" || id == "ac_chroma_keyer" || id == "ac_plate_keyer";
}
obs_source_t *nativeFilter(obs_source_t *source)
{
    obs_source_t *found = nullptr;
    obs_source_enum_filters(source, [](obs_source_t *, obs_source_t *child, void *opaque) {
        auto &out = *static_cast<obs_source_t **>(opaque);
        if (!out && std::string(obs_source_get_unversioned_id(child)) == FilterId) out = obs_source_get_ref(child);
    }, &found);
    return found;
}
class Dock final : public QWidget {
public:
    Dock();
    ~Dock() override;
    void shutdown();
    void setupWindow();
private:
    void floatWindow();
    void setFullscreen(bool enabled);
    void refreshSources();
    void chooseSource();
    void detach();
    void tick();
    void start(AnalysisMode mode);
    void cancel(bool reload = true);
    void finish();
    void apply();
    void revert(bool reset = false);
    void reset();
    void loadControls();
    void updateRegions();
    void updateCandidate();
    void updateButtons();
    void updateStrokeList();
    void rememberStrokes();
    void status(const QString &text);
    bool locked() const { return streamStarting_.load() || recordStarting_.load() || isLive(); }
    void invalidateAnalysisRegion();
    bool writable();
    bool attached() const;
    QString savePlate(const Frame &frame);
    Roi roi() const;
    QJsonObject candidateSettings() const;
    static void frontend(enum obs_frontend_event event, void *data);
    QComboBox *sources_, *views_, *region_, *regionTool_, *engine_, *zoom_;
    QPushButton *editRegion_;
    QPointer<QDockWidget> dockWindow_;
    QPushButton *fullscreen_;
    QByteArray windowGeometry_;
    Qt::WindowStates windowState_;
    bool windowWasFloating_ = false;
    QElapsedTimer sceneTransformTimer_;
    PreviewWidget *preview_;
    QLabel *status_, *info_;
    QPushButton *automatic_, *plate_, *apply_, *cancel_, *revert_, *reset_;
    QPushButton *clearRegion_;
    QProgressBar *progress_;
    QSpinBox *similarity_, *smoothness_;
    QDoubleSpinBox *despill_, *tau_;
    QDoubleSpinBox *precisionTolerance_, *precisionSoftness_, *precisionEdge_, *precisionFeather_;
    QCheckBox *despeck_, *localStroke_;
    QListWidget *strokeList_;
    QPushButton *addStroke_, *excludeStroke_, *deleteStroke_, *undoStroke_, *clearStrokes_, *editStrokeScope_;
    QJsonArray strokes_;
    std::vector<QJsonArray> strokeHistory_;
    int scopeEditStroke_ = -1;
    QTimer timer_;
    QFutureWatcher<AnalysisResult> watcher_;
    PreviewDriver driver_;
    obs_source_t *source_ = nullptr, *filter_ = nullptr;
    std::vector<Frame> frames_;
    CancelFlag cancelFlag_;
    AnalysisResult pending_;
    QString pendingPlate_;
    Rect analysis_ {0, 0, 1, 1};
    Region protect_, erase_, local_;
    uint64_t lastStamp_ = 0;
    int required_ = 0, collectedTicks_ = 0;
    AnalysisMode mode_ = AnalysisMode::Auto;
    bool collecting_ = false, analyzing_ = false, ready_ = false, shuttingDown_ = false;
    bool loading_ = false, createdFilter_ = false;
    int generation_ = 0, workerGeneration_ = 0;
    uint32_t width_ = 0, height_ = 0;
    uint32_t workerWidth_ = 0, workerHeight_ = 0;
    std::atomic_bool streamStarting_{false}, recordStarting_{false};
};
QPointer<Dock> dock;

Dock::Dock()
{
    auto *outer = new QVBoxLayout(this);
    auto *splitter = new QSplitter(Qt::Horizontal, this); splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(11);
    auto *handleStyle = "QSplitter::handle:horizontal { background: palette(mid); margin: 0 3px; }"
                        "QSplitter::handle:horizontal:hover { background: palette(highlight); }";
    splitter->setStyleSheet(QString::fromLatin1(handleStyle));
    auto *viewer = new QWidget(splitter); auto *viewerLayout = new QVBoxLayout(viewer); viewerLayout->setContentsMargins(0, 0, 0, 0);
    viewer->setMinimumWidth(280);
    auto *scroll = new QScrollArea(splitter); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setMinimumWidth(260);
    auto *content = new QWidget(scroll);
    auto *layout = new QVBoxLayout(content); scroll->setWidget(content);
    splitter->addWidget(viewer); splitter->addWidget(scroll); splitter->setStretchFactor(0, 3); splitter->setStretchFactor(1, 2); splitter->setSizes({660, 420});
    splitter->handle(1)->setToolTip(QStringLiteral("끌어서 미리보기와 조정 메뉴의 너비를 바꿉니다."));
    outer->addWidget(splitter, 1);
    auto *title = new QLabel(QStringLiteral("자동 크로마키"), this);
    title->setStyleSheet("font-size: 17px; font-weight: 600;"); layout->addWidget(title);
    auto *row = new QHBoxLayout;
    sources_ = new QComboBox(this); sources_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    sources_->setMinimumContentsLength(10); row->addWidget(sources_, 1);
    auto *refresh = new QPushButton(QStringLiteral("새로고침"), this); row->addWidget(refresh); layout->addLayout(row);
    info_ = new QLabel(QStringLiteral("고정 카메라 · SDR 색 배경지 촬영"), this); info_->setWordWrap(true); layout->addWidget(info_);
    engine_ = new QComboBox(this); engine_->addItems({QStringLiteral("기본 모드 · 기존 GPU 엔진"), QStringLiteral("정밀 모드 · 스트로크 다중 색상")}); layout->addWidget(engine_);
    views_ = new QComboBox(this);
    views_->addItems({QStringLiteral("원본"), QStringLiteral("처리 결과 · 체커"), QStringLiteral("알파 (투명도)"), QStringLiteral("배경 합성 미리보기"), QStringLiteral("처리 결과 · 검정"), QStringLiteral("처리 결과 · 흰색")});
    views_->setCurrentIndex(1);
    auto *viewRow = new QHBoxLayout;
    const QString viewNames[] {QStringLiteral("원본"), QStringLiteral("결과"), QStringLiteral("알파")};
    for (int i = 0; i < 3; ++i) {
        auto *button = new QPushButton(viewNames[i]); button->setCheckable(true); button->setChecked(i == 1);
        connect(button, &QPushButton::clicked, this, [this, i, button] { views_->setCurrentIndex(i); button->setChecked(true); });
        connect(views_, &QComboBox::currentIndexChanged, button, [button, i](int index) { button->setChecked(index == i); });
        viewRow->addWidget(button);
    }
    viewerLayout->addLayout(viewRow); viewerLayout->addWidget(views_);
    preview_ = new PreviewWidget(this); viewerLayout->addWidget(preview_, 1);
    auto *zoomRow = new QHBoxLayout;
    auto *zoomOut = new QPushButton(QStringLiteral("−")); zoomOut->setMaximumWidth(40); zoomOut->setToolTip(QStringLiteral("축소"));
    auto *zoomIn = new QPushButton(QStringLiteral("+")); zoomIn->setMaximumWidth(40); zoomIn->setToolTip(QStringLiteral("확대"));
    auto *fit = new QPushButton(QStringLiteral("화면 맞춤"));
    zoom_ = new QComboBox(this); zoom_->addItems({QStringLiteral("화면에 맞춤"), QStringLiteral("25%"), QStringLiteral("50%"), QStringLiteral("100%"), QStringLiteral("200%"), QStringLiteral("400%"), QStringLiteral("800%")});
    zoom_->setEditable(true); zoom_->lineEdit()->setReadOnly(true);
    zoomRow->addWidget(zoomOut); zoomRow->addWidget(zoom_, 1); zoomRow->addWidget(zoomIn); zoomRow->addWidget(fit); viewerLayout->addLayout(zoomRow);
    auto *windowRow = new QHBoxLayout;
    auto *floating = new QPushButton(QStringLiteral("분리 창")); auto *maximize = new QPushButton(QStringLiteral("최대화"));
    fullscreen_ = new QPushButton(QStringLiteral("전체화면")); fullscreen_->setCheckable(true);
    windowRow->addWidget(floating); windowRow->addWidget(maximize); windowRow->addWidget(fullscreen_); viewerLayout->addLayout(windowRow);
    connect(floating, &QPushButton::clicked, this, [this] { if (fullscreen_->isChecked()) fullscreen_->setChecked(false); floatWindow(); });
    connect(maximize, &QPushButton::clicked, this, [this] {
        if (fullscreen_->isChecked()) fullscreen_->setChecked(false);
        floatWindow(); if (dockWindow_) { if (dockWindow_->isMaximized()) dockWindow_->showNormal(); else dockWindow_->showMaximized(); }
    });
    connect(fullscreen_, &QPushButton::toggled, this, [this](bool enabled) { setFullscreen(enabled); });
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this); escape->setContext(Qt::WidgetWithChildrenShortcut);
    escape->setEnabled(false);
    connect(fullscreen_, &QPushButton::toggled, escape, &QShortcut::setEnabled);
    connect(escape, &QShortcut::activated, this, [this] { fullscreen_->setChecked(false); });
    auto *viewerHint = new QLabel(QStringLiteral("OBS 회전·자르기·크기 반영 · 휠 확대/축소 · 휠 버튼으로 이동\n가운데 분할선을 끌어 미리보기와 메뉴 너비를 조절합니다."));
    viewerHint->setWordWrap(true); viewerLayout->addWidget(viewerHint);
    row = new QHBoxLayout;
    region_ = new QComboBox(this);
    region_->addItems({QStringLiteral("영역 편집 안 함"), QStringLiteral("분석 영역 그리기"), QStringLiteral("강제 보호 영역 그리기"), QStringLiteral("강제 지움 영역 그리기"), QStringLiteral("새 스트로크 국소 범위 그리기"), QStringLiteral("배경색 추가 스트로크 +"), QStringLiteral("선택 색 제외 스트로크 −")});
    row->addWidget(region_, 1);
    clearRegion_ = new QPushButton(QStringLiteral("영역 지우기"), this); row->addWidget(clearRegion_); layout->addLayout(row);
    row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("그리기 도구"), this));
    regionTool_ = new QComboBox(this);
    regionTool_->addItems({QStringLiteral("사각형"), QStringLiteral("원"), QStringLiteral("타원"), QStringLiteral("펜")});
    regionTool_->setToolTip(QStringLiteral("펜은 클릭으로 점을 만들고 드래그로 곡선 핸들을 만듭니다. 시작점 근처를 클릭하거나 Enter로 닫습니다."));
    row->addWidget(regionTool_, 1); layout->addLayout(row);
    editRegion_ = new QPushButton(QStringLiteral("기존 영역 편집"), this);
    editRegion_->setCheckable(true);
    editRegion_->setToolTip(QStringLiteral("Alt를 누르고 핸들을 끌면 한쪽만 움직입니다. 윤곽 클릭으로 점을 추가하고, 점을 선택해 Delete로 삭제합니다."));
    layout->addWidget(editRegion_);
    auto *background = new QPushButton(QStringLiteral("합성 미리보기 배경 사진 선택"), this); layout->addWidget(background);
    auto *hint = new QLabel(QStringLiteral("정밀: 부족한 배경에 선을 추가 → 알파 확인 → Despill → 결과 적용\n강제 지움은 인물도 자를 수 있습니다. 배경 영상은 카메라 아래에 배치하세요."), this);
    hint->setWordWrap(true); layout->addWidget(hint);
    auto *strokeGroup = new QGroupBox(QStringLiteral("스트로크 색 선택 · 최대 16개"), this);
    auto *strokeLayout = new QVBoxLayout(strokeGroup);
    row = new QHBoxLayout;
    addStroke_ = new QPushButton(QStringLiteral("배경색 추가 +")); excludeStroke_ = new QPushButton(QStringLiteral("선택 색 제외 −"));
    row->addWidget(addStroke_); row->addWidget(excludeStroke_); strokeLayout->addLayout(row);
    localStroke_ = new QCheckBox(QStringLiteral("새 스트로크를 국소 보정 범위 안에서만 적용")); strokeLayout->addWidget(localStroke_);
    strokeList_ = new QListWidget; strokeList_->setMaximumHeight(100); strokeLayout->addWidget(strokeList_);
    editStrokeScope_ = new QPushButton(QStringLiteral("선택한 국소 스트로크 범위 수정")); strokeLayout->addWidget(editStrokeScope_);
    row = new QHBoxLayout;
    deleteStroke_ = new QPushButton(QStringLiteral("선택 삭제")); undoStroke_ = new QPushButton(QStringLiteral("실행 취소")); clearStrokes_ = new QPushButton(QStringLiteral("모두 지우기"));
    row->addWidget(deleteStroke_); row->addWidget(undoStroke_); row->addWidget(clearStrokes_); strokeLayout->addLayout(row); layout->addWidget(strokeGroup);
    row = new QHBoxLayout;
    automatic_ = new QPushButton(QStringLiteral("자동 맞춤"), this);
    plate_ = new QPushButton(QStringLiteral("빈 배경 학습 (선택)"), this);
    row->addWidget(automatic_); row->addWidget(plate_); layout->insertLayout(3, row);
    row = new QHBoxLayout;
    apply_ = new QPushButton(QStringLiteral("결과 적용"), this); cancel_ = new QPushButton(QStringLiteral("취소"), this);
    revert_ = new QPushButton(QStringLiteral("되돌리기"), this); reset_ = new QPushButton(QStringLiteral("초기화"), this);
    row->addWidget(apply_); row->addWidget(cancel_); row->addWidget(revert_); row->addWidget(reset_); layout->insertLayout(4, row);
    auto *advanced = new QGroupBox(QStringLiteral("고급 조정 (적용 전 미리보기)"), this);
    advanced->setCheckable(true); advanced->setChecked(false);
    auto *advancedBody = new QWidget(advanced);
    auto *advancedLayout = new QVBoxLayout(advanced); advancedLayout->addWidget(advancedBody);
    auto *form = new QFormLayout(advancedBody);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    similarity_ = new QSpinBox; similarity_->setRange(1, 1000); similarity_->setValue(400);
    smoothness_ = new QSpinBox; smoothness_->setRange(1, 1000); smoothness_->setValue(80);
    despill_ = new QDoubleSpinBox; despill_->setRange(0, 1); despill_->setSingleStep(0.05); despill_->setValue(1);
    tau_ = new QDoubleSpinBox; tau_->setRange(0.5, 10); tau_->setSingleStep(0.1); tau_->setValue(3);
    auto precisionSpin = [](double lo, double hi, double value) { auto *s = new QDoubleSpinBox; s->setDecimals(3); s->setRange(lo, hi); s->setSingleStep(.005); s->setValue(value); return s; };
    precisionTolerance_ = precisionSpin(.005, .30, .025); precisionSoftness_ = precisionSpin(.005, .30, .06);
    precisionEdge_ = precisionSpin(0, 1, .5); precisionFeather_ = precisionSpin(0, .2, .02);
    despeck_ = new QCheckBox(QStringLiteral("작은 고립점 제거 (빈 배경 학습만 · 기본 꺼짐)"));
    form->addRow(QStringLiteral("유사성 (기본 색 키)"), sliderControl(similarity_)); form->addRow(QStringLiteral("매끄러움 (기본 색 키)"), sliderControl(smoothness_));
    form->addRow(QStringLiteral("Despill · 배경색 번짐 제거"), sliderControl(despill_)); form->addRow(QStringLiteral("배경판 색 차이 허용"), sliderControl(tau_));
    form->addRow(QStringLiteral("정밀 · 색 허용 범위"), sliderControl(precisionTolerance_)); form->addRow(QStringLiteral("정밀 · 경계 부드러움"), sliderControl(precisionSoftness_));
    form->addRow(QStringLiteral("정밀 · 머리카락 경계 보정"), sliderControl(precisionEdge_)); form->addRow(QStringLiteral("국소 범위 경계 부드러움"), sliderControl(precisionFeather_)); form->addRow(despeck_);
    advancedBody->setVisible(false);
    connect(advanced, &QGroupBox::toggled, advancedBody, &QWidget::setVisible);
    layout->addWidget(advanced);
    progress_ = new QProgressBar(this); progress_->setRange(0, 100); progress_->setValue(0); progress_->setMaximumHeight(18); outer->addWidget(progress_);
    status_ = new QLabel(QStringLiteral("대기 중 · 소스를 선택하고 자동 맞춤을 누르세요."), this);
    status_->setWordWrap(true); status_->setMaximumHeight(72); status_->setTextInteractionFlags(Qt::TextSelectableByMouse); outer->addWidget(status_);
    auto *credit = new QLabel(QStringLiteral("제작자 SunjooAn · v%1").arg(utf8(Version)), this);
    credit->setWordWrap(true); outer->addWidget(credit);
    connect(refresh, &QPushButton::clicked, this, [this] { refreshSources(); });
    connect(sources_, &QComboBox::currentIndexChanged, this, [this] { chooseSource(); });
    connect(views_, &QComboBox::currentIndexChanged, preview_, &PreviewWidget::setView);
    connect(zoom_, &QComboBox::activated, this, [this](int index) { const int values[] {0, 25, 50, 100, 200, 400, 800}; preview_->setZoom(values[std::clamp(index, 0, 6)]); });
    preview_->zoomChanged = [this](double percent) { QSignalBlocker block(zoom_); zoom_->setCurrentIndex(-1); zoom_->setEditText(QStringLiteral("%1%").arg(percent, 0, 'f', 1)); };
    connect(zoomOut, &QPushButton::clicked, this, [this] {
        const double percent = std::max(5., preview_->zoomPercent() / 1.2); preview_->setZoom(percent); preview_->zoomChanged(percent);
    });
    connect(zoomIn, &QPushButton::clicked, this, [this] {
        const double percent = std::min(800., preview_->zoomPercent() * 1.2); preview_->setZoom(percent); preview_->zoomChanged(percent);
    });
    connect(fit, &QPushButton::clicked, this, [this] { zoom_->setCurrentIndex(0); preview_->setZoom(0); });
    connect(engine_, &QComboBox::currentIndexChanged, this, [this] { if (!loading_) { scopeEditStroke_ = -1; preview_->cancelInteraction(); if (engine_->currentIndex() == 0 && region_->currentIndex() >= 5) region_->setCurrentIndex(0); updateCandidate(); updateButtons(); } });
    connect(region_, &QComboBox::currentIndexChanged, this, [this](int index) {
        scopeEditStroke_ = -1;
        editRegion_->setChecked(false);
        if (index >= 5 && engine_->currentIndex() == 0) engine_->setCurrentIndex(1);
        preview_->setEditRegion(index);
        updateButtons();
    });
    connect(regionTool_, &QComboBox::currentIndexChanged, this, [this](int index) {
        editRegion_->setChecked(false);
        preview_->setRegionTool(RegionTool(index));
        if (index == int(RegionTool::Pen)) status(QStringLiteral("점을 클릭하거나 드래그해 곡선을 만드세요. 시작점 근처 클릭 또는 Enter로 닫습니다."));
    });
    connect(editRegion_, &QPushButton::toggled, this, [this](bool enabled) {
        preview_->setExistingRegionEdit(enabled);
        if (enabled) status(QStringLiteral("윤곽 클릭으로 점 추가 · 점 선택 후 Delete · Alt+핸들 드래그로 한쪽 조절"));
    });
    connect(addStroke_, &QPushButton::clicked, this, [this] { engine_->setCurrentIndex(1); region_->setCurrentIndex(5); });
    connect(excludeStroke_, &QPushButton::clicked, this, [this] { engine_->setCurrentIndex(1); region_->setCurrentIndex(6); });
    connect(localStroke_, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && !loading_ && !local_.valid()) { region_->setCurrentIndex(4); status(QStringLiteral("국소 보정 범위를 그린 뒤 배경색 추가 스트로크를 선택하세요.")); }
    });
    connect(strokeList_, &QListWidget::currentRowChanged, this, [this](int index) {
        if (scopeEditStroke_ >= 0) editRegion_->setChecked(false);
        scopeEditStroke_ = -1; preview_->setSelectedStroke(index); updateButtons();
    });
    connect(editStrokeScope_, &QPushButton::clicked, this, [this] {
        const int index = strokeList_->currentRow();
        if (!writable() || collecting_ || analyzing_ || index < 0 || index >= strokes_.size()) return;
        const auto stroke = strokes_[index].toObject(); const auto box = stroke["rect"].toArray();
        if (!stroke["local"].toBool() || box.size() != 4) return;
        region_->setCurrentIndex(4); scopeEditStroke_ = index;
        local_ = readRegion(stroke["region_points"], {box[0].toDouble(), box[1].toDouble(), box[2].toDouble(), box[3].toDouble()});
        local_.controls = decodeControls(stroke["region_edit"]);
        updateRegions();
        updateButtons();
        editRegion_->setChecked(local_.valid());
        status(QStringLiteral("선택한 스트로크의 국소 범위입니다. 점·핸들을 끌어 수정하세요."));
    });
    connect(deleteStroke_, &QPushButton::clicked, this, [this] {
        if (!writable() || collecting_ || analyzing_) return;
        const int index = strokeList_->currentRow(); if (index < 0 || index >= strokes_.size()) return;
        scopeEditStroke_ = -1; rememberStrokes(); strokes_.removeAt(index); updateStrokeList(); updateCandidate();
    });
    connect(clearStrokes_, &QPushButton::clicked, this, [this] {
        if (!writable() || collecting_ || analyzing_ || strokes_.isEmpty()) return;
        rememberStrokes(); strokes_ = {}; updateStrokeList(); updateCandidate();
    });
    connect(undoStroke_, &QPushButton::clicked, this, [this] {
        if (!writable() || collecting_ || analyzing_ || strokeHistory_.empty()) return;
        strokes_ = strokeHistory_.back(); strokeHistory_.pop_back(); updateStrokeList(); updateCandidate();
    });
    connect(strokeList_, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (loading_ || locked() || collecting_ || analyzing_) return;
        const int index = strokeList_->row(item); if (index < 0 || index >= strokes_.size()) return;
        rememberStrokes(); auto stroke = strokes_[index].toObject(); stroke["enabled"] = item->checkState() == Qt::Checked; strokes_[index] = stroke;
        preview_->setStrokes(strokes_); updateCandidate();
    });
    preview_->strokeCompleted = [this](QJsonObject stroke) {
        if (!writable() || collecting_ || analyzing_) return;
        if (strokes_.size() >= 16) { status(QStringLiteral("스트로크는 최대 16개입니다. 필요 없는 선을 삭제하거나 실행 취소하세요.")); return; }
        if (localStroke_->isChecked() && !local_.valid()) { status(QStringLiteral("국소 보정 범위를 먼저 그려 주세요.")); return; }
        rememberStrokes(); stroke["local"] = localStroke_->isChecked();
        const auto box = local_.bounds;
        stroke["rect"] = QJsonArray {box.x, box.y, box.width, box.height};
        if (localStroke_->isChecked()) {
            if (!local_.outline.isEmpty()) stroke["region_points"] = regionPoints(local_);
            if (!local_.controls.anchors.isEmpty()) stroke["region_edit"] = encodeControls(local_.controls);
        }
        strokes_.append(stroke);
        updateStrokeList(); updateCandidate();
        status(QStringLiteral("색 샘플을 추가했습니다. 알파와 머리카락 경계를 확인하고 결과 적용을 누르세요. 선을 더 그릴 수 있습니다."));
    };
    connect(clearRegion_, &QPushButton::clicked, this, [this] {
        if (collecting_ || analyzing_) return;
        editRegion_->setChecked(false);
        if (region_->currentIndex() == 1) { analysis_ = {0, 0, 1, 1}; invalidateAnalysisRegion(); }
        if (region_->currentIndex() == 2) protect_ = {};
        if (region_->currentIndex() == 3) erase_ = {};
        if (region_->currentIndex() == 4) { local_ = {}; scopeEditStroke_ = -1; }
        updateRegions(); if (region_->currentIndex() != 1) updateCandidate(); updateButtons();
    });
    preview_->regionChanged = [this](int region, Region value) {
        if (region == 1) { analysis_ = value.bounds; invalidateAnalysisRegion(); }
        if (region == 2) protect_ = value;
        if (region == 3) erase_ = value;
        if (region == 4) {
            local_ = value;
            if (scopeEditStroke_ >= 0 && scopeEditStroke_ < strokes_.size()) {
                const int index = scopeEditStroke_;
                rememberStrokes(); auto stroke = strokes_[index].toObject();
                const auto box = value.bounds;
                stroke["rect"] = QJsonArray {box.x, box.y, box.width, box.height};
                if (value.outline.isEmpty()) stroke.remove("region_points");
                else stroke["region_points"] = regionPoints(value);
                stroke["region_edit"] = encodeControls(value.controls);
                strokes_[index] = stroke;
                updateStrokeList();
                scopeEditStroke_ = index;
                status(QStringLiteral("선택한 스트로크의 국소 범위를 수정했습니다. 미리보기 확인 후 결과 적용을 누르세요."));
            }
        }
        updateRegions(); if (region != 1) updateCandidate(); updateButtons();
    };
    connect(background, &QPushButton::clicked, this, [this] {
        const auto file = QFileDialog::getOpenFileName(this, QStringLiteral("배경 사진 선택"), {}, QStringLiteral("사진 (*.png *.jpg *.jpeg *.bmp)"));
        if (file.isEmpty()) return;
        QImageReader reader(file); reader.setAutoTransform(true);
        const auto size = reader.size();
        if (!size.isValid() || qint64(size.width()) * size.height() > 100000000) { status(QStringLiteral("배경 사진 크기를 확인해 주세요.")); return; }
        if (size.width() > 1920 || size.height() > 1920) reader.setScaledSize(size.scaled(1920, 1920, Qt::KeepAspectRatio));
        auto image = reader.read();
        if (image.isNull()) { status(QStringLiteral("사진을 읽지 못했습니다.")); return; }
        preview_->setBackground(image); views_->setCurrentIndex(3);
    });
    connect(automatic_, &QPushButton::clicked, this, [this] { start(AnalysisMode::Auto); });
    connect(plate_, &QPushButton::clicked, this, [this] { start(AnalysisMode::Plate); });
    connect(apply_, &QPushButton::clicked, this, [this] { apply(); });
    connect(cancel_, &QPushButton::clicked, this, [this] { cancel(); });
    connect(revert_, &QPushButton::clicked, this, [this] { revert(); });
    connect(reset_, &QPushButton::clicked, this, [this] { reset(); });
    connect(similarity_, &QSpinBox::valueChanged, this, [this] { updateCandidate(); });
    connect(smoothness_, &QSpinBox::valueChanged, this, [this] { updateCandidate(); });
    connect(despill_, &QDoubleSpinBox::valueChanged, this, [this] { updateCandidate(); });
    connect(tau_, &QDoubleSpinBox::valueChanged, this, [this] { updateCandidate(); });
    for (auto *control : {precisionTolerance_, precisionSoftness_, precisionEdge_, precisionFeather_})
        connect(control, &QDoubleSpinBox::valueChanged, this, [this] { updateCandidate(); });
    connect(despeck_, &QCheckBox::toggled, this, [this] { updateCandidate(); });
    connect(&watcher_, &QFutureWatcher<AnalysisResult>::finished, this, [this] {
        try { finish(); }
        catch (const std::exception &) { analyzing_ = false; cancel(); status(QStringLiteral("분석 결과를 준비하지 못했습니다. 해상도를 낮추고 다시 시도하세요.")); }
    });
    connect(&timer_, &QTimer::timeout, this, [this] {
        try { tick(); }
        catch (const std::exception &) { cancel(); requestCapture(filter_, false); status(QStringLiteral("화면 처리 메모리가 부족합니다. 해상도를 낮추고 다시 시도하세요.")); }
    });
    timer_.start(40); obs_frontend_add_event_callback(frontend, this);
    refreshSources(); updateButtons();
}
Dock::~Dock() { shutdown(); }
void Dock::setupWindow()
{
    for (QWidget *parent = parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto *window = qobject_cast<QDockWidget *>(parent)) { dockWindow_ = window; break; }
    }
    if (!dockWindow_) return;
    dockWindow_->setFeatures(dockWindow_->features() | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetMovable);
    dockWindow_->setWindowTitle(QStringLiteral("자동 크로마키"));
}
void Dock::floatWindow()
{
    if (!dockWindow_) return;
    if (!dockWindow_->isFloating()) {
        dockWindow_->setFloating(true);
        const auto available = dockWindow_->screen()->availableGeometry().size();
        dockWindow_->resize(QSize(1100, 850).boundedTo(available));
    }
    dockWindow_->show(); dockWindow_->raise();
}
void Dock::setFullscreen(bool enabled)
{
    if (!dockWindow_) return;
    preview_->cancelInteraction();
    if (enabled) {
        windowWasFloating_ = dockWindow_->isFloating(); windowGeometry_ = dockWindow_->saveGeometry(); windowState_ = dockWindow_->windowState();
        floatWindow(); dockWindow_->showFullScreen(); fullscreen_->setText(QStringLiteral("전체화면 종료"));
    } else {
        dockWindow_->showNormal(); dockWindow_->setFloating(windowWasFloating_);
        if (windowWasFloating_) { dockWindow_->restoreGeometry(windowGeometry_); dockWindow_->setWindowState(windowState_); }
        dockWindow_->show(); fullscreen_->setText(QStringLiteral("전체화면"));
    }
}
void Dock::shutdown()
{
    if (shuttingDown_) return;
    shuttingDown_ = true; timer_.stop();
    obs_frontend_remove_event_callback(frontend, this);
    if (cancelFlag_) cancelFlag_->store(true);
    watcher_.waitForFinished(); detach();
    // OBS 32 can report allocator leaks before Qt destroys extra docks.
    driver_.stop();
}
void Dock::status(const QString &text) { status_->setText(text); }
void Dock::invalidateAnalysisRegion()
{
    cancel(false); status(QStringLiteral("분석 영역이 바뀌었습니다. 자동 맞춤을 다시 실행하세요."));
}
void Dock::refreshSources()
{
    const auto selected = sources_->currentData().toString();
    QSignalBlocker block(sources_); sources_->clear(); sources_->addItem(QStringLiteral("카메라 소스 선택"), QString());
    obs_enum_sources([](void *opaque, obs_source_t *source) {
        if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) && !obs_source_is_scene(source) && !obs_source_is_group(source))
            static_cast<QComboBox *>(opaque)->addItem(utf8(obs_source_get_name(source)), utf8(obs_source_get_uuid(source)));
        return true;
    }, sources_);
    const int index = sources_->findData(selected); sources_->setCurrentIndex(std::max(0, index));
}
void Dock::detach()
{
    preview_->cancelInteraction(); strokes_ = {}; strokeHistory_.clear(); updateStrokeList();
    ++generation_; collecting_ = false; ready_ = false; frames_.clear();
    if (!pendingPlate_.isEmpty()) QFile::remove(pendingPlate_);
    pendingPlate_.clear();
    if (cancelFlag_) cancelFlag_->store(true);
    driver_.setSource(nullptr);
    if (filter_) { requestCapture(filter_, false); setPreviewSettings(filter_, nullptr); obs_source_release(filter_); filter_ = nullptr; }
    if (source_) { obs_source_dec_showing(source_); obs_source_release(source_); source_ = nullptr; }
    width_ = height_ = 0; createdFilter_ = false;
}
bool Dock::attached() const
{
    if (!source_ || !filter_ || obs_source_removed(source_)) return false;
    auto *found = nativeFilter(source_); const bool same = found == filter_;
    if (found) obs_source_release(found);
    return same;
}
void Dock::chooseSource()
{
    detach(); preview_->setFrames({});
    const auto uuid = sources_->currentData().toString().toUtf8();
    if (uuid.isEmpty()) { updateButtons(); return; }
    source_ = obs_get_source_by_uuid(uuid.constData());
    if (!source_) { status(QStringLiteral("소스를 찾지 못했습니다. 새로고침하세요.")); updateButtons(); return; }
    obs_source_inc_showing(source_);
    filter_ = nativeFilter(source_);
    if (!filter_) {
        if (locked()) { status(QStringLiteral("방송·녹화를 마친 뒤 처음 연결해 주세요.")); obs_source_dec_showing(source_); obs_source_release(source_); source_ = nullptr; updateButtons(); return; }
        obs_data_t *settings = obs_data_create(); obs_data_set_string(settings, "method", "passthrough");
        filter_ = obs_source_create(FilterId, "자동 크로마키", settings, nullptr); obs_data_release(settings);
        if (filter_) { obs_source_filter_add(source_, filter_); obs_source_filter_set_order(source_, filter_, OBS_ORDER_MOVE_TOP); createdFilter_ = true; }
    }
    if (!filter_) { status(QStringLiteral("필터를 만들지 못했습니다. OBS 로그를 확인하세요.")); detach(); updateButtons(); return; }
    requestCapture(filter_, true); driver_.setSource(source_); loadControls();
    sceneTransformTimer_.invalidate(); preview_->setSceneTransform(sourceTransform(source_)); preview_->setZoom(0); zoom_->setCurrentIndex(0);
    status(QStringLiteral("자동 맞춤 → 미리보기 확인 → 결과 적용 순서로 진행하세요.")); updateButtons();
}
void Dock::loadControls()
{
    if (!filter_) return;
    loading_ = true;
    auto *s = obs_source_get_settings(filter_);
    engine_->setCurrentIndex(std::string(obs_data_get_string(s, "method")) == "precision" ? 1 : 0);
    if (engine_->currentIndex() == 0 && region_->currentIndex() >= 5) region_->setCurrentIndex(0);
    similarity_->setValue(int(obs_data_get_int(s, "similarity"))); smoothness_->setValue(int(obs_data_get_int(s, "smoothness")));
    despill_->setValue(obs_data_get_double(s, "despill_strength")); tau_->setValue(obs_data_get_double(s, "tau_cd"));
    despeck_->setChecked(obs_data_get_bool(s, "despeck"));
    auto number = [s](const char *key, double fallback) { return obs_data_has_user_value(s, key) || obs_data_has_default_value(s, key) ? obs_data_get_double(s, key) : fallback; };
    precisionTolerance_->setValue(number("precision_tolerance", .025)); precisionSoftness_->setValue(number("precision_softness", .06));
    precisionEdge_->setValue(number("precision_edge", .5)); precisionFeather_->setValue(number("precision_feather", .02));
    const QByteArray serialized(obs_data_get_string(s, "precision_strokes"));
    const auto savedStrokes = serialized.size() <= 256 * 1024 ? QJsonDocument::fromJson(serialized).array() : QJsonArray();
    strokes_ = {};
    for (const auto &value : savedStrokes) {
        if (strokes_.size() >= 16) break;
        auto stroke = value.toObject(); QJsonArray points, samples;
        for (const auto &sample : stroke["samples"].toArray()) {
            if (samples.size() >= 64) break;
            const auto rgb = sample.toArray(); bool valid = rgb.size() == 3;
            for (const auto &channel : rgb) valid = valid && channel.isDouble() && std::isfinite(channel.toDouble()) && channel.toDouble() >= 0 && channel.toDouble() <= 255;
            if (valid) samples.append(rgb);
        }
        if (samples.isEmpty()) continue;
        for (const auto &point : stroke["points"].toArray()) {
            if (points.size() >= 64) break;
            const auto xy = point.toArray();
            if (xy.size() == 2 && xy[0].isDouble() && xy[1].isDouble() && std::isfinite(xy[0].toDouble()) && std::isfinite(xy[1].toDouble()))
                points.append(QJsonArray {std::clamp(xy[0].toDouble(), 0., 1.), std::clamp(xy[1].toDouble(), 0., 1.)});
        }
        stroke["samples"] = samples; stroke["points"] = points; strokes_.append(stroke);
    }
    strokeHistory_.clear(); preview_->cancelInteraction();
    auto rect = [s](const char *prefix) {
        const std::string p(prefix); return Rect {obs_data_get_double(s, (p + "_x").c_str()), obs_data_get_double(s, (p + "_y").c_str()),
            obs_data_get_double(s, (p + "_w").c_str()), obs_data_get_double(s, (p + "_h").c_str())};
    };
    protect_ = parseRegion(obs_data_get_string(s, "protect_points"), rect("protect"));
    erase_ = parseRegion(obs_data_get_string(s, "erase_points"), rect("erase"));
    local_ = parseRegion(obs_data_get_string(s, "precision_local_points"), rect("precision_local"));
    auto controls = [s](const char *key) {
        const QByteArray json(obs_data_get_string(s, key));
        return json.size() <= 16384 ? decodeControls(QJsonDocument::fromJson(json).object()) : RegionControls{};
    };
    protect_.controls = controls("protect_edit");
    erase_.controls = controls("erase_edit");
    local_.controls = controls("precision_local_edit");
    editRegion_->setChecked(false);
    analysis_ = rect("analysis");
    if (!local_.valid()) localStroke_->setChecked(false);
    if (analysis_.width <= 0 || analysis_.height <= 0) analysis_ = {0, 0, 1, 1};
    obs_data_release(s); loading_ = false; updateRegions(); updateStrokeList(); updateButtons();
}
void Dock::updateRegions() { preview_->setRegions(analysis_, protect_, erase_, local_); }
void Dock::rememberStrokes()
{
    if (strokeHistory_.size() >= 32) strokeHistory_.erase(strokeHistory_.begin());
    strokeHistory_.push_back(strokes_);
}
void Dock::updateStrokeList()
{
    scopeEditStroke_ = -1;
    QSignalBlocker blocker(strokeList_);
    const int selected = strokeList_->currentRow(); strokeList_->clear();
    for (int i = 0; i < strokes_.size(); ++i) {
        const auto stroke = strokes_[i].toObject();
        auto *item = new QListWidgetItem(QStringLiteral("%1. %2 · %3 · %4색").arg(i + 1)
            .arg(stroke["subtract"].toBool() ? QStringLiteral("선택 제외 −") : QStringLiteral("배경 추가 +"))
            .arg(stroke["local"].toBool() ? QStringLiteral("국소") : QStringLiteral("전체"))
            .arg(stroke["samples"].toArray().size()), strokeList_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable); item->setCheckState(stroke["enabled"].toBool(true) ? Qt::Checked : Qt::Unchecked);
    }
    if (!strokes_.isEmpty()) strokeList_->setCurrentRow(std::clamp(selected, 0, int(strokes_.size()) - 1));
    preview_->setStrokes(strokes_); preview_->setSelectedStroke(strokeList_->currentRow());
}
Roi Dock::roi() const
{
    return {int(std::round(analysis_.y * height_)), int(std::round((1 - analysis_.y - analysis_.height) * height_)),
            int(std::round(analysis_.x * width_)), int(std::round((1 - analysis_.x - analysis_.width) * width_))};
}
bool Dock::writable()
{
    if (locked()) { status(QStringLiteral("방송·녹화 중에는 보정과 적용을 할 수 없습니다.")); return false; }
    if (!attached()) { status(QStringLiteral("카메라 소스를 다시 선택해 주세요.")); return false; }
    return true;
}
void Dock::start(AnalysisMode mode)
{
    if (!writable() || analyzing_ || collecting_) return;
    const int dialogGeneration = generation_;
    if (mode == AnalysisMode::Plate && QMessageBox::question(this, QStringLiteral("빈 배경 학습"),
        QStringLiteral("사람·손·그림자와 보여 줄 물건을 화면 밖으로 치우세요.\n지금 화면에 있는 진열대·소품도 배경으로 저장되어 지워집니다.\n빈 배경을 학습할까요?")) != QMessageBox::Yes) return;
    if (dialogGeneration != generation_ || !writable()) return;
    if (!obs_source_enabled(filter_)) { status(QStringLiteral("소스의 필터 목록에서 자동 크로마키를 켠 뒤 다시 시도하세요.")); return; }
    const uint64_t bytes = uint64_t(width_) * height_ * 4 * (mode == AnalysisMode::Auto ? 5 : 30);
    if (bytes > 512ULL * 1024 * 1024) { status(QStringLiteral("프레임 수집 메모리 한도(512MiB)를 넘습니다. 빈 배경 학습은 1080p로 사용해 주세요.")); return; }
    cancel(false);
    if (mode == AnalysisMode::Plate) { loading_ = true; engine_->setCurrentIndex(0); loading_ = false; }
    mode_ = mode; required_ = mode == AnalysisMode::Auto ? 5 : 30;
    frames_.clear(); lastStamp_ = 0; collectedTicks_ = 0; collecting_ = true; ready_ = false;
    cancelFlag_ = std::make_shared<std::atomic_bool>(false);
    progress_->setRange(0, required_); progress_->setValue(0);
    status(QStringLiteral("프레임 수집 중… 카메라와 조명을 고정해 주세요.")); updateButtons();
}
void Dock::cancel(bool reload)
{
    ++generation_;
    if (cancelFlag_) cancelFlag_->store(true);
    collecting_ = false; ready_ = false; frames_.clear();
    if (!pendingPlate_.isEmpty()) QFile::remove(pendingPlate_);
    pendingPlate_.clear();
    if (filter_) setPreviewSettings(filter_, nullptr);
    preview_->cancelInteraction();
    if (reload && filter_) loadControls();
    progress_->setRange(0, 100); progress_->setValue(0);
    status(analyzing_ ? QStringLiteral("취소 처리 중… 계산이 멈추면 다시 실행할 수 있습니다.") : QStringLiteral("취소했습니다. 적용된 설정은 유지됩니다."));
    updateButtons();
}
void Dock::tick()
{
    if (shuttingDown_) return;
    if (source_ && !attached()) { cancel(); detach(); preview_->setFrames({}); status(QStringLiteral("소스 또는 필터가 제거되었습니다. 다시 선택하세요.")); updateButtons(); return; }
    updateButtons();
    if (!filter_) return;
    if (uint64_t(obs_source_get_base_width(source_)) * obs_source_get_base_height(source_) > 3840ULL * 2160ULL) {
        if (collecting_ || analyzing_ || ready_) cancel();
        requestCapture(filter_, false); driver_.setSource(nullptr);
        status(QStringLiteral("미리보기는 최대 4K 픽셀 수까지 지원합니다. 소스 해상도를 낮춰 주세요.")); return;
    }
    if (!obs_source_enabled(filter_)) {
        status(QStringLiteral("자동 크로마키 필터가 꺼져 있습니다. 소스의 필터 목록에서 켜 주세요.")); return;
    }
    const bool running = isVisible() || collecting_ || ready_;
    requestCapture(filter_, running); driver_.setSource(running ? source_ : nullptr);
    if (!running) return;
    if (!sceneTransformTimer_.isValid() || sceneTransformTimer_.elapsed() >= 250) {
        preview_->setSceneTransform(sourceTransform(source_)); sceneTransformTimer_.start();
    }
    auto p = readPreview(filter_);
    if (!p.original.valid()) {
        if (collecting_ && ++collectedTicks_ > 250) { cancel(); status(QStringLiteral("10초 동안 화면을 읽지 못했습니다. 카메라 연결과 소스 표시를 확인하세요.")); }
        if (!p.error.empty()) status(utf8(p.error.c_str()));
        return;
    }
    if ((width_ && width_ != p.original.width) || (height_ && height_ != p.original.height)) {
        preview_->cancelInteraction();
        if (collecting_ || analyzing_ || ready_) { cancel(); status(QStringLiteral("해상도가 바뀌었습니다. 자동 맞춤을 다시 실행하세요.")); }
    }
    width_ = p.original.width; height_ = p.original.height;
    info_->setText(QStringLiteral("%1 · %2 × %3 · %4").arg(utf8(obs_source_get_name(source_))).arg(width_).arg(height_).arg(locked() ? QStringLiteral("송출 잠금") : QStringLiteral("준비 모드")));
    preview_->setFrames(p);
    if (!collecting_) return;
    if (uint64_t(p.original.rgba.size()) * required_ > 512ULL * 1024 * 1024) {
        cancel(); status(QStringLiteral("프레임 수집 메모리 한도(512MiB)를 넘습니다. 빈 배경 학습은 1080p로 사용해 주세요.")); return;
    }
    if (++collectedTicks_ > 750) { cancel(); status(QStringLiteral("프레임 수집 시간이 초과됐습니다. 다시 시도하세요.")); return; }
    if (p.original.timestamp == lastStamp_) return;
    if (locked()) { cancel(); return; }
    lastStamp_ = p.original.timestamp; frames_.push_back(std::move(p.original)); progress_->setValue(int(frames_.size()));
    if (int(frames_.size()) < required_) return;
    collecting_ = false; analyzing_ = true; workerGeneration_ = generation_;
    workerWidth_ = width_; workerHeight_ = height_;
    progress_->setRange(0, 0); status(QStringLiteral("배경을 분석하는 중… 결과는 적용 전까지 송출에 반영되지 않습니다."));
    char *module = obs_module_file("lua");
    const std::string moduleDir = module ? QFileInfo(utf8(module)).absoluteFilePath().toUtf8().toStdString() : "";
    bfree(module);
    const auto lua = QDir(QCoreApplication::applicationDirPath()).filePath("lua51.dll").toUtf8().toStdString();
    auto frames = std::move(frames_); const auto r = roi(); const auto m = mode_; const auto flag = cancelFlag_;
    watcher_.setFuture(QtConcurrent::run([frames = std::move(frames), r, m, flag, moduleDir, lua]() { return analyze(frames, r, m, flag, moduleDir, lua); }));
    updateButtons();
}
QString Dock::savePlate(const Frame &frame)
{
    if (!frame.valid()) return {};
    char *path = obs_module_config_path("plates");
    const QString directory = path ? QFileInfo(utf8(path)).absoluteFilePath() : QString(); bfree(path);
    if (directory.isEmpty() || !QDir().mkpath(directory)) return {};
    const auto name = utf8(obs_source_get_uuid(source_)) + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".png";
    const auto fileName = QDir(directory).filePath(name);
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly)) return {};
    QImage image(frame.rgba.data(), int(frame.width), int(frame.height), int(frame.width * 4), QImage::Format_RGBA8888);
    if (!image.save(&file, "PNG") || !file.commit()) return {};
    return fileName;
}
void Dock::finish()
{
    analyzing_ = false;
    if (shuttingDown_) return;
    progress_->setRange(0, 100); progress_->setValue(0);
    auto result = watcher_.result();
    if (workerGeneration_ != generation_ || !attached() || locked() || result.cancelled ||
        width_ != workerWidth_ || height_ != workerHeight_ ||
        (result.plate.valid() && (result.plate.width != width_ || result.plate.height != height_))) {
        status(QStringLiteral("분석 결과를 취소했습니다. 기존 설정은 유지됩니다.")); updateButtons(); return;
    }
    if (!result.success) { status(QStringLiteral("적용하지 않음: ") + utf8(result.message.c_str())); updateButtons(); return; }
    pendingPlate_.clear();
    if (result.plate.valid()) {
        pendingPlate_ = savePlate(result.plate);
        if (pendingPlate_.isEmpty()) { status(QStringLiteral("배경판을 저장하지 못했습니다. 기존 설정은 유지됩니다.")); updateButtons(); return; }
    }
    pending_ = std::move(result); ready_ = true; loading_ = true;
    if (pending_.numbers.count("similarity")) similarity_->setValue(int(pending_.numbers["similarity"]));
    if (pending_.numbers.count("smoothness")) smoothness_->setValue(int(pending_.numbers["smoothness"]));
    if (pending_.numbers.count("despill_strength")) despill_->setValue(pending_.numbers["despill_strength"]);
    if (pending_.numbers.count("tau_cd")) tau_->setValue(pending_.numbers["tau_cd"]);
    despeck_->setChecked(false); loading_ = false;
    updateCandidate(); views_->setCurrentIndex(1); progress_->setValue(100);
    status(utf8(pending_.message.c_str()) + QStringLiteral("\n분석 결과를 미리보기로 확인하고 ‘결과 적용’을 누르세요."));
    updateButtons();
}
QJsonObject Dock::candidateSettings() const
{
    auto obj = settingsJson(filter_);
    obj["method"] = engine_->currentIndex() == 1 ? QStringLiteral("precision") : utf8(pending_.method.c_str());
    for (const auto &[key, value] : pending_.numbers) obj[utf8(key.c_str())] = value;
    for (const auto &[key, value] : pending_.strings) obj[utf8(key.c_str())] = utf8(value.c_str());
    if (!pendingPlate_.isEmpty()) obj["plate_path"] = pendingPlate_;
    obj["similarity"] = similarity_->value(); obj["smoothness"] = smoothness_->value();
    obj["despill_strength"] = despill_->value(); obj["tau_cd"] = tau_->value();
    obj["despeck"] = despeck_->isChecked() && obj["method"].toString() == "plate" && obj["plate_kind"].toString() == "manual";
    obj["precision_strokes"] = QString::fromUtf8(QJsonDocument(strokes_).toJson(QJsonDocument::Compact));
    obj["precision_tolerance"] = precisionTolerance_->value(); obj["precision_softness"] = precisionSoftness_->value();
    obj["precision_edge"] = precisionEdge_->value(); obj["precision_feather"] = precisionFeather_->value();
    auto rect = [&obj](const char *prefix, Rect r) {
        const QString p = utf8(prefix); obj[p + "_x"] = r.x; obj[p + "_y"] = r.y; obj[p + "_w"] = r.width; obj[p + "_h"] = r.height;
    };
    rect("analysis", analysis_);
    auto region = [&obj, &rect](const char *prefix, const Region &value) {
        rect(prefix, value.bounds);
        obj[utf8(prefix) + "_points"] = QString::fromUtf8(QJsonDocument(regionPoints(value)).toJson(QJsonDocument::Compact));
        obj[utf8(prefix) + "_edit"] = QString::fromUtf8(QJsonDocument(encodeControls(value.controls)).toJson(QJsonDocument::Compact));
    };
    region("protect", protect_); region("erase", erase_); region("precision_local", local_);
    obj["schema_version"] = 5; return obj;
}
void Dock::updateCandidate()
{
    if (loading_ || !filter_ || collecting_ || analyzing_ || locked()) return;
    if (!ready_) {
        const auto current = settingsJson(filter_);
        const auto method = current.value("method").toString();
        if (method != "color" && method != "plate" && method != "precision" && engine_->currentIndex() == 0) return;
        pending_ = {}; pending_.success = true; pending_.method = (method == "precision" || method == "passthrough" || method.isEmpty()) ? "color" : method.toStdString(); ready_ = true;
    }
    auto *s = fromJson(candidateSettings()); setPreviewSettings(filter_, s); obs_data_release(s);
    updateButtons();
}
void Dock::apply()
{
    if (!ready_ || !writable()) return;
    auto obj = candidateSettings();
    const auto previous = settingsJson(filter_);
    if (previous.value("ac_backup").toString().isEmpty()) {
        auto original = previous; original.remove("ac_backup"); original.remove("ac_disabled_keyers");
        if (!original.contains("method")) original["method"] = "passthrough";
        obj["ac_backup"] = QString::fromUtf8(QJsonDocument(original).toJson(QJsonDocument::Compact));
    }
    QJsonArray keyers = QJsonDocument::fromJson(previous.value("ac_disabled_keyers").toString().toUtf8()).array();
    obs_source_enum_filters(source_, [](obs_source_t *, obs_source_t *child, void *opaque) {
        if (!isKeyer(child)) return;
        auto &array = *static_cast<QJsonArray *>(opaque);
        const auto uuid = utf8(obs_source_get_uuid(child));
        for (const auto &entry : array) if (entry.toObject()["uuid"].toString() == uuid) return;
        array.append(QJsonObject {{"uuid", uuid}, {"name", utf8(obs_source_get_name(child))},
            {"id", utf8(obs_source_get_unversioned_id(child))}, {"enabled", obs_source_enabled(child)}});
    }, &keyers);
    obj["ac_disabled_keyers"] = QString::fromUtf8(QJsonDocument(keyers).toJson(QJsonDocument::Compact));
    auto *s = fromJson(obj); obs_source_update(filter_, s); obs_data_release(s);
    obs_source_enum_filters(source_, [](obs_source_t *, obs_source_t *child, void *) { if (isKeyer(child)) obs_source_set_enabled(child, false); }, nullptr);
    obs_source_set_enabled(filter_, true); setPreviewSettings(filter_, nullptr); ready_ = false; pendingPlate_.clear();
    loadControls(); status(QStringLiteral("적용했습니다. 기존 크로마키는 꺼 두었으며 ‘되돌리기’로 복원할 수 있습니다.")); updateButtons();
}
void Dock::revert(bool reset)
{
    if (!attached()) return;
    cancel(); const auto current = settingsJson(filter_);
    const auto backup = current.value("ac_backup").toString();
    if (backup.isEmpty()) {
        if (reset) { obs_data_t *s = obs_data_create(); obs_data_set_string(s, "method", "passthrough"); obs_source_reset_settings(filter_, s); obs_data_release(s); loadControls(); }
        status(QStringLiteral("복원할 적용 기록이 없습니다.")); return;
    }
    const auto keyers = QJsonDocument::fromJson(current.value("ac_disabled_keyers").toString().toUtf8()).array();
    bool missing = false;
    for (const auto &entry : keyers) {
        const auto record = entry.toObject();
        struct Lookup { QString uuid; obs_source_t *found = nullptr; } lookup {record["uuid"].toString()};
        obs_source_enum_filters(source_, [](obs_source_t *, obs_source_t *item, void *opaque) {
            auto &value = *static_cast<Lookup *>(opaque);
            if (utf8(obs_source_get_uuid(item)) == value.uuid) value.found = obs_source_get_ref(item);
        }, &lookup);
        auto *child = lookup.found;
        if (!child || utf8(obs_source_get_uuid(child)) != record["uuid"].toString() || utf8(obs_source_get_unversioned_id(child)) != record["id"].toString()) missing = true;
        else obs_source_set_enabled(child, record["enabled"].toBool());
        if (child) obs_source_release(child);
    }
    auto original = QJsonDocument::fromJson(backup.toUtf8()).object();
    if (missing) { original["ac_backup"] = backup; original["ac_disabled_keyers"] = current["ac_disabled_keyers"]; }
    auto *s = fromJson(original); obs_source_reset_settings(filter_, s); obs_data_release(s); loadControls();
    status(missing ? QStringLiteral("일부 기존 필터를 찾지 못했습니다. 복원 기록을 보존했습니다.") : QStringLiteral("적용 전 설정과 기존 크로마키 상태를 복원했습니다."));
}
void Dock::reset()
{
    if (!writable() || analyzing_) return;
    const int dialogGeneration = generation_;
    if (QMessageBox::question(this, QStringLiteral("자동 크로마키 초기화"),
        QStringLiteral("자동 크로마키 설정을 초기화하고 기존 필터 상태를 복원할까요?\n촬영 원본과 다른 소스의 설정은 유지됩니다.")) != QMessageBox::Yes) return;
    if (dialogGeneration != generation_ || !writable()) return;
    revert(true);
    auto *s = obs_source_get_settings(filter_);
    const bool unresolved = *obs_data_get_string(s, "ac_backup") != '\0'; obs_data_release(s);
    if (unresolved) return;
    obs_source_filter_remove(source_, filter_); detach(); preview_->setFrames({});
    sources_->setCurrentIndex(0); status(QStringLiteral("초기화했습니다. 소스를 다시 선택해 시작하세요.")); updateButtons();
}
void Dock::updateButtons()
{
    const bool busy = collecting_ || analyzing_, active = source_ && filter_;
    const bool editable = active && !busy && !locked(), precision = engine_->currentIndex() == 1;
    std::string selectedMethod = ready_ ? pending_.method : "";
    bool manualPlate = false;
    if (filter_) {
        auto *settings = obs_source_get_settings(filter_);
        if (!ready_) selectedMethod = obs_data_get_string(settings, "method");
        manualPlate = std::string(obs_data_get_string(settings, "plate_kind")) == "manual";
        obs_data_release(settings);
    }
    if (ready_ && pending_.strings.count("plate_kind")) manualPlate = pending_.strings.at("plate_kind") == "manual";
    const bool color = !precision && selectedMethod == "color", plate = !precision && selectedMethod == "plate";
    sources_->setEnabled(!busy); automatic_->setEnabled(active && !busy && !locked());
    plate_->setEnabled(active && !busy && !locked()); apply_->setEnabled(active && ready_ && !busy && !locked());
    cancel_->setEnabled(busy || ready_); revert_->setEnabled(active && !analyzing_); reset_->setEnabled(active && !busy && !locked());
    region_->setEnabled(!busy && !locked()); clearRegion_->setEnabled(!busy && !locked()); preview_->setEnabled(!busy && !locked());
    regionTool_->setEnabled(editable && region_->currentIndex() >= 2 && region_->currentIndex() <= 4);
    const int regionIndex = region_->currentIndex();
    const bool hasRegion = (regionIndex == 2 && protect_.valid()) || (regionIndex == 3 && erase_.valid()) ||
                           (regionIndex == 4 && local_.valid());
    if (!hasRegion) editRegion_->setChecked(false);
    editRegion_->setEnabled(editable && hasRegion);
    if (!editable) preview_->cancelInteraction();
    engine_->setEnabled(editable); strokeList_->setEnabled(editable && precision); localStroke_->setEnabled(editable && precision);
    const int selectedStroke = strokeList_->currentRow();
    editStrokeScope_->setEnabled(editable && precision && selectedStroke >= 0 && selectedStroke < strokes_.size() && strokes_[selectedStroke].toObject()["local"].toBool());
    addStroke_->setEnabled(editable); excludeStroke_->setEnabled(editable); deleteStroke_->setEnabled(editable && precision && !strokes_.isEmpty());
    clearStrokes_->setEnabled(editable && precision && !strokes_.isEmpty()); undoStroke_->setEnabled(editable && precision && !strokeHistory_.empty());
    similarity_->parentWidget()->setEnabled(editable && color); smoothness_->parentWidget()->setEnabled(editable && color);
    despill_->parentWidget()->setEnabled(editable && (precision || color || plate)); tau_->parentWidget()->setEnabled(editable && plate);
    for (auto *control : {precisionTolerance_, precisionSoftness_, precisionEdge_, precisionFeather_}) control->parentWidget()->setEnabled(editable && precision);
    despeck_->setEnabled(editable && plate && manualPlate);
}
void Dock::frontend(enum obs_frontend_event event, void *opaque)
{
    auto *self = static_cast<Dock *>(opaque);
    if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING) self->streamStarting_.store(true);
    if (event == OBS_FRONTEND_EVENT_RECORDING_STARTING) self->recordStarting_.store(true);
    if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED || event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) self->streamStarting_.store(false);
    if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED || event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) self->recordStarting_.store(false);
    // EXIT is the last opportunity to release OBS resources, so handle it synchronously on the frontend thread.
    const auto action = [self, event] {
        if (self->shuttingDown_) return;
        if (event == OBS_FRONTEND_EVENT_EXIT) { self->shutdown(); return; }
        if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING || event == OBS_FRONTEND_EVENT_RECORDING_STARTING) {
            if (self->collecting_ || self->analyzing_ || self->ready_) self->cancel();
            self->status(QStringLiteral("방송·녹화가 시작되어 설정 변경을 잠갔습니다."));
        }
        if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING) {
            self->cancel(); self->detach(); self->preview_->setFrames({});
        }
        if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) self->refreshSources();
        self->updateButtons();
    };
    if (QThread::currentThread() == self->thread()) action();
    else QMetaObject::invokeMethod(self, action, event == OBS_FRONTEND_EVENT_EXIT ? Qt::BlockingQueuedConnection : Qt::QueuedConnection);
}
}
void createDock()
{
    if (dock || !QApplication::instance() || !obs_frontend_get_main_window()) return;
    dock = new Dock;
    dock->resize(1100, 850);
    if (!obs_frontend_add_dock_by_id(DockId, "자동 크로마키", dock)) { delete dock; dock = nullptr; }
    else dock->setupWindow();
}
void destroyDock()
{
    if (!dock) return;
    dock->shutdown();
    obs_frontend_remove_dock(DockId);
    // OBS owns the dock widget. Removal schedules its Qt destruction.
    dock = nullptr;
}
}
