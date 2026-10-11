package com.adisplay.tv.ui

import androidx.compose.runtime.Composable
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.darkColorScheme

/**
 * 电视端主题。
 *
 * 固定用深色：电视通常放在客厅，深夜看片时浅色界面刺眼，
 * 而且绝大多数电视端的系统 UI 也是深色，保持一致更协调。
 */
@Composable
fun ADisplayTvTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = darkColorScheme(),
        content = content
    )
}
