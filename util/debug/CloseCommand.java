import java.util.*;

import command.*;

public class CloseCommand implements Command
{
	public String getName()
	{
		return "close";
	}
	
	public String getHelp()
	{
		return "Close currently open file.";
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		if(data == null)
			System.out.println("No file loaded.");
		return null;
	}
}
