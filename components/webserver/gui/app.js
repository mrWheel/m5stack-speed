function currentStore()
{
  return document.querySelector('input[name="store"]:checked').value;
}

function setStatus(message)
{
  document.getElementById("statusMessage").textContent = message;
}

function formatSize(bytes)
{
  if (bytes < 1024)
  {
    return bytes + " B";
  }
  return (bytes / 1024).toFixed(1) + " kB";
}

function refreshFileList()
{
  fetch("/api/files?store=" + currentStore())
    .then(function (response) { return response.json(); })
    .then(function (files)
    {
      var body = document.getElementById("fileTableBody");
      body.innerHTML = "";
      files.forEach(function (file)
      {
        var row = document.createElement("tr");

        var nameCell = document.createElement("td");
        nameCell.textContent = file.name;
        row.appendChild(nameCell);

        var sizeCell = document.createElement("td");
        sizeCell.textContent = formatSize(file.size);
        row.appendChild(sizeCell);

        var actionCell = document.createElement("td");

        var downloadLink = document.createElement("a");
        downloadLink.href = "/api/download?store=" + currentStore() + "&name=" + encodeURIComponent(file.name);
        downloadLink.textContent = "Download";
        downloadLink.style.marginRight = "1em";
        actionCell.appendChild(downloadLink);

        var deleteButton = document.createElement("button");
        deleteButton.textContent = "Delete";
        deleteButton.onclick = function ()
        {
          deleteFile(file.name);
        };
        actionCell.appendChild(deleteButton);

        row.appendChild(actionCell);
        body.appendChild(row);
      });
    })
    .catch(function (error)
    {
      setStatus("Failed to load file list: " + error);
    });
}

function deleteFile(name)
{
  fetch("/api/delete?store=" + currentStore() + "&name=" + encodeURIComponent(name), { method: "DELETE" })
    .then(function ()
    {
      setStatus("Deleted " + name);
      refreshFileList();
    })
    .catch(function (error)
    {
      setStatus("Failed to delete " + name + ": " + error);
    });
}

function uploadFile()
{
  var input = document.getElementById("uploadFile");
  if (input.files.length === 0)
  {
    setStatus("Select a file first.");
    return;
  }

  var file = input.files[0];
  fetch("/api/upload?store=" + currentStore() + "&name=" + encodeURIComponent(file.name), {
    method: "POST",
    body: file,
  })
    .then(function ()
    {
      setStatus("Uploaded " + file.name);
      input.value = "";
      refreshFileList();
    })
    .catch(function (error)
    {
      setStatus("Failed to upload " + file.name + ": " + error);
    });
}

document.getElementById("uploadButton").addEventListener("click", uploadFile);
document.querySelectorAll('input[name="store"]').forEach(function (radio)
{
  radio.addEventListener("change", refreshFileList);
});

refreshFileList();
