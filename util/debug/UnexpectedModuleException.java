public class UnexpectedModuleException extends BadInputException
{
	public final short moduleNumber;
	
	public UnexpectedModuleException(short moduleNumber, long offset)
	{
		super("Unexpected module: " + Module.hex(moduleNumber), offset);
		this.moduleNumber = moduleNumber;
	}
}
