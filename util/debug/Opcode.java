import java.util.Vector;

public abstract class Opcode implements Constants
{
	protected String file = null;
	protected int line = 0;
	protected String function = null;
	protected UniqueStack stack = null;
	
	public void setFile(String file)
	{
		if(this.file != null)
			throw new RuntimeException("File name already set!");
		this.file = file;
	}
	
	public void setLine(int line)
	{
		if(this.line != 0)
			throw new RuntimeException("Line number already set!");
		this.line = line;
	}
	
	public void setFunction(String function)
	{
		if(this.function != null)
			throw new RuntimeException("Function name already set!");
		this.function = function;
	}
	
	public void setStack(UniqueStack stack)
	{
		if(this.stack != null)
			throw new RuntimeException("Stack trace already set!");
		this.stack = stack;
	}
	
	public String getFile()
	{
		return file;
	}
	
	public int getLine()
	{
		return line;
	}
	
	public String getFunction()
	{
		return function;
	}
	
	public UniqueStack getStack()
	{
		return stack;
	}
	
	public abstract void applyTo(SystemState state);
	
	public boolean hasEffect()
	{
		return true;
	}
	
	public boolean isSkippable()
	{
		return false;
	}
	
	public abstract String toString();
	
	public static String hex(short opcodeNumber)
	{
		String hex = Integer.toHexString(opcodeNumber);
		while(hex.length() < 4)
			hex = "0" + hex;
		return "0x" + hex;
	}
}
