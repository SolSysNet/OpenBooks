package org.openbooks.android

import org.json.JSONArray
import org.json.JSONObject
import org.openbooks.core.NativeBooks

/** An error reported by the engine; [code] is "password_required", "wrong_password" or "error". */
class CoreException(val code: String, message: String) : Exception(message)

/**
 * Typed access to the native engine. Every call is synchronous and must run off the main
 * thread (see [AppState.run] and [rememberLoad]). Money is passed as exact decimal strings.
 */
object Core {
    fun call(op: String, vararg args: Pair<String, Any?>): Any? {
        val request = jsonOf(*args).put("op", op)
        val reply = JSONObject(NativeBooks.call(request.toString()))
        if (!reply.getBoolean("ok")) {
            throw CoreException(reply.optString("code", "error"), reply.optString("error", "unknown error"))
        }
        return reply.opt("data")
    }

    fun obj(op: String, vararg args: Pair<String, Any?>): JSONObject = call(op, *args) as JSONObject
    fun arr(op: String, vararg args: Pair<String, Any?>): JSONArray = call(op, *args) as JSONArray
    fun int(op: String, vararg args: Pair<String, Any?>): Int = (call(op, *args) as Number).toInt()
}

/** Builds a JSON object; lists become arrays, maps become objects, null is left out. */
fun jsonOf(vararg pairs: Pair<String, Any?>): JSONObject {
    val o = JSONObject()
    for ((k, v) in pairs) {
        if (v != null) o.put(k, toJson(v))
    }
    return o
}

private fun toJson(v: Any): Any = when (v) {
    is JSONObject, is JSONArray, is String, is Int, is Long, is Boolean -> v
    is List<*> -> JSONArray().apply { v.filterNotNull().forEach { put(toJson(it)) } }
    is Map<*, *> -> JSONObject().apply { v.forEach { (k, x) -> if (x != null) put(k.toString(), toJson(x)) } }
    else -> v.toString()
}

fun JSONArray.objects(): List<JSONObject> = List(length()) { getJSONObject(it) }
fun JSONArray.strings(): List<String> = List(length()) { getString(it) }
fun JSONArray.ints(): List<Int> = List(length()) { getInt(it) }
fun JSONObject.s(key: String): String = optString(key, "")
fun JSONObject.i(key: String): Int = optInt(key, 0)
fun JSONObject.b(key: String): Boolean = optBoolean(key, false)
fun JSONObject.list(key: String): List<JSONObject> = optJSONArray(key)?.objects() ?: emptyList()

// ---------------------------------------------------------------- money

object MoneyFmt {
    /** "-1234.5" -> "-1,234.50". Works on the digits, never on floating point. */
    fun format(amount: String): String {
        var t = amount.trim()
        if (t.isEmpty()) return ""
        val negative = t.startsWith("-")
        if (negative) t = t.substring(1)
        val dot = t.indexOf('.')
        val whole = if (dot >= 0) t.substring(0, dot) else t
        val frac = (if (dot >= 0) t.substring(dot + 1) else "").padEnd(2, '0').take(2)
        val grouped = whole.reversed().chunked(3).joinToString(",").reversed().ifEmpty { "0" }
        return (if (negative) "-" else "") + grouped + "." + frac
    }

    fun isNegative(amount: String) = amount.trim().startsWith("-") && amount.any { it in '1'..'9' }
    fun isZero(amount: String) = amount.none { it in '1'..'9' }

    /** Accepts what people type: "1,234.5", "$20", "(15.00)". Returns a plain decimal or null. */
    fun parse(text: String): String? {
        var t = text.trim().replace(",", "").replace("$", "")
        if (t.isEmpty()) return null
        var negative = false
        if (t.startsWith("(") && t.endsWith(")")) {
            negative = true
            t = t.substring(1, t.length - 1)
        }
        if (t.startsWith("-")) {
            negative = !negative
            t = t.substring(1)
        }
        if (!Regex("""\d+(\.\d{0,2})?|\.\d{1,2}""").matches(t)) return null
        return (if (negative) "-" else "") + t
    }
}
