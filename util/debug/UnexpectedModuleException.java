public class UnexpectedModuleException extends BadInputException
{
	private final short moduleNumber;
	
	public UnexpectedModuleException(short moduleNumber)
	{
		super("Unexpected module: " + Module.hex(moduleNumber));
		this.moduleNumber = moduleNumber;
	}
	
	public short getModuleNumber()
	{
		return moduleNumber;
	}
}
