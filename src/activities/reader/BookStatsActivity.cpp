#include "BookStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "BookStatsView.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                     const BookReadingStats& initialStats, std::string initialBookCachePath,
                                     const GlobalReadingStats& initialGlobalStats, const InitialPage initialPage)
    : Activity("BookStats", renderer, mappedInput),
      bookTitle(std::move(title)),
      bookCachePath(std::move(initialBookCachePath)),
      stats(initialStats),
      globalStats(initialGlobalStats),
      page(initialPage == InitialPage::Achievement ? Page::Achievement : Page::Summary) {}

void BookStatsActivity::onEnter() {
  Activity::onEnter();
  // The card grid is laid out for portrait; it does not fit landscape heights.
  previousOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // Truncate the book title once for the activity's lifetime. render() can
  // run repeatedly (one call per display refresh) and truncating a long
  // title on every frame would allocate a std::string + heap copy on each
  // call. bookTitle and screenWidth are both fixed from this point on, so
  // a single onEnter() computation is sufficient.
  if (!bookTitle.empty()) {
    truncatedTitle =
        renderer.truncatedText(UI_12_FONT_ID, bookTitle.c_str(), renderer.getScreenWidth() - 20, EpdFontFamily::BOLD);
    const int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, truncatedTitle.c_str(), EpdFontFamily::BOLD);
    titleX = (renderer.getScreenWidth() - titleWidth) / 2;
  } else {
    titleX = -1;  // sentinel: render() falls back to tr(STR_READING_STATS) which is centered by the renderer
  }
  // First paint is ours to request (see GlobalStatsActivity::onEnter).
  requestUpdate();
}

void BookStatsActivity::onExit() {
  saveStats();
  renderer.setOrientation(previousOrientation);
  Activity::onExit();
}

void BookStatsActivity::cycleEditField() { selectedEditField = (selectedEditField + 1) % 6; }

ReadingStatsDate BookStatsActivity::defaultDateForField(const bool finishedField) const {
  if (finishedField && stats.finishedDate.isValid()) {
    return stats.finishedDate;
  }
  if (!finishedField && stats.startDate.isValid()) {
    return stats.startDate;
  }
  if (finishedField && stats.startDate.isValid()) {
    return stats.startDate;
  }
  if (!finishedField && stats.finishedDate.isValid()) {
    return stats.finishedDate;
  }

  ReadingStatsDateTime now;
  if (getCurrentLocalReadingStatsDateTime(now)) {
    return now.date;
  }
  return ReadingStatsDate{2000, 1, 1};
}

void BookStatsActivity::applyCompletedState(const bool completed) {
  if (stats.isCompleted == completed) {
    return;
  }

  stats.isCompleted = completed;
  stats.completionAchievementPending = completed;
  stats.completionPromptDismissedAtHundred = false;
  if (completed) {
    globalStats.completedBooks++;
    if (!stats.finishedDateManual && !stats.finishedDate.isValid()) {
      ReadingStatsDateTime now;
      if (getCurrentLocalReadingStatsDateTime(now)) {
        stats.finishedDate = now.date;
      }
    }
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }
}

void BookStatsActivity::normalizeEditedDates(const bool editedFinishedField) {
  if (!stats.startDate.isValid() || !stats.finishedDate.isValid()) {
    return;
  }
  if (compareReadingStatsDate(stats.finishedDate, stats.startDate) >= 0) {
    return;
  }

  if (editedFinishedField) {
    stats.startDate = stats.finishedDate;
  } else {
    stats.finishedDate = stats.startDate;
  }
}

void BookStatsActivity::clearEditedDate(const bool finishedField) {
  ReadingStatsDate& date = finishedField ? stats.finishedDate : stats.startDate;
  date.clear();

  if (finishedField) {
    stats.finishedDateManual = false;
    applyCompletedState(false);
  } else {
    stats.startDateManual = false;
  }

  didChangeStats = true;
  requestUpdate();
}

bool BookStatsActivity::shouldClearDateOnAdjust(const ReadingStatsDate& date, const bool finishedField,
                                                const int fieldIndex, const int delta) const {
  if (!date.isValid()) {
    return false;
  }

  switch (fieldIndex) {
    case 0:
      return (date.month == 1 && delta < 0) || (date.month == 12 && delta > 0);
    case 1: {
      const uint8_t monthDays = daysInMonth(date.year, date.month);
      return (date.day == 1 && delta < 0) || (date.day == monthDays && delta > 0);
    }
    case 2:
      return (date.year == 2000 && delta < 0) || (date.year == 2099 && delta > 0);
    default:
      return false;
  }
}

void BookStatsActivity::adjustSelectedDateField(const int delta) {
  const bool finishedField = selectedEditField >= 3;
  ReadingStatsDate& date = finishedField ? stats.finishedDate : stats.startDate;
  const int fieldIndex = selectedEditField % 3;

  if (shouldClearDateOnAdjust(date, finishedField, fieldIndex, delta)) {
    clearEditedDate(finishedField);
    return;
  }

  if (!date.isValid()) {
    date = defaultDateForField(finishedField);
  }

  switch (fieldIndex) {
    case 0: {
      int month = static_cast<int>(date.month) + delta;
      while (month < 1) {
        month += 12;
      }
      while (month > 12) {
        month -= 12;
      }
      date.month = static_cast<uint8_t>(month);
      break;
    }
    case 1: {
      const int monthDays = daysInMonth(date.year, date.month);
      int day = static_cast<int>(date.day) + delta;
      while (day < 1) {
        day += monthDays;
      }
      while (day > monthDays) {
        day -= monthDays;
      }
      date.day = static_cast<uint8_t>(day);
      break;
    }
    case 2: {
      int year = static_cast<int>(date.year) + delta;
      if (year < 2000) {
        year = 2099;
      } else if (year > 2099) {
        year = 2000;
      }
      date.year = static_cast<uint16_t>(year);
      break;
    }
    default:
      break;
  }

  const uint8_t monthDays = daysInMonth(date.year, date.month);
  if (date.day > monthDays) {
    date.day = monthDays;
  }

  if (finishedField) {
    stats.finishedDateManual = true;
    applyCompletedState(true);
  } else {
    stats.startDateManual = true;
  }
  normalizeEditedDates(finishedField);

  didChangeStats = true;
  requestUpdate();
}

void BookStatsActivity::saveStats() {
  if (!didChangeStats || !hasEditableBook()) {
    return;
  }

  stats.save(bookCachePath);
  globalStats.save();
  didChangeStats = false;
}

void BookStatsActivity::loop() {
  if (page == Page::EditDates) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      saveStats();
      page = Page::Summary;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      cycleEditField();
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      adjustSelectedDateField(-1);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      adjustSelectedDateField(1);
      return;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (page == Page::Achievement) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  // More (Right/Down) opens the date editor; Confirm keeps the reader-facing
  // Clear-pace contract so the existing result handler stays authoritative.
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (hasEditableBook()) {
      page = Page::EditDates;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(ActivityResult{ClearPaceResult{}});
    finish();
  }
}

void BookStatsActivity::render(RenderLock&&) {
  if (page == Page::EditDates) {
    BookStatsView::renderEditBookDatesPage(renderer, &mappedInput, bookTitle, stats, selectedEditField,
                                           /*showButtonHints=*/true);
    renderer.displayBuffer();
    return;
  }
  if (page == Page::Achievement) {
    BookStatsView::renderReadingAchievementPage(renderer, &mappedInput, bookTitle, stats, globalStats,
                                                /*showButtonHints=*/true);
    renderer.displayBuffer();
    return;
  }

  const float progressPercent = stats.lastBookProgressPercent == UNKNOWN_BOOK_PROGRESS_PERCENT
                                    ? -1.0f
                                    : static_cast<float>(stats.lastBookProgressPercent);
  BookStatsView::renderPerBookStatsPage(renderer, &mappedInput, bookTitle, stats, progressPercent,
                                        /*hasEstimatedTimeLeft=*/false, stats.estimatedTimeLeftSeconds,
                                        /*showButtonHints=*/true);

  const char* title = bookTitle.empty() ? tr(STR_READING_STATS) : truncatedTitle.c_str();
  if (titleX >= 0 && !bookTitle.empty()) {
    renderer.drawText(UI_12_FONT_ID, titleX, 15, title, true, EpdFontFamily::BOLD);
  } else if (bookTitle.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, 15, title, true, EpdFontFamily::BOLD);
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_CLEAR_BOOK_PACE), "", hasEditableBook() ? tr(STR_MORE) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
